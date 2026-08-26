#include "catalog_internal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <system_error>
#include <utility>

#include "ravo/domain/uri.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/profile_gamma.h"

namespace ravo
{

[[nodiscard]] std::int64_t now_unix_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] ImportItemResult failed_item(std::string path, TaskError error)
{
    ImportItemResult result;
    result.status = ImportItemStatus::kFailed;
    result.input_path = std::move(path);
    result.error = std::move(error);
    return result;
}

[[nodiscard]] ImportItemResult unsupported_item(std::string path, TaskError error)
{
    ImportItemResult result;
    result.status = ImportItemStatus::kUnsupported;
    result.input_path = std::move(path);
    result.error = std::move(error);
    return result;
}

[[nodiscard]] std::string lower_ascii(std::string value)
{
    for (char &character : value)
    {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return value;
}

[[nodiscard]] std::string extension_lower(const std::filesystem::path &path)
{
    const auto extension = path.extension().generic_u8string();
    return lower_ascii({reinterpret_cast<const char *>(extension.data()), extension.size()});
}

[[nodiscard]] bool is_raw_extension(const std::filesystem::path &path)
{
    static const std::set<std::string> raw{".arw", ".cr2", ".cr3", ".crw", ".nef", ".nrw", ".dng",
                                           ".raf", ".orf", ".rw2", ".raw", ".sr2", ".srf", ".pef",
                                           ".3fr", ".mrw", ".kdc", ".dcr", ".erf"};
    return raw.contains(extension_lower(path));
}

[[nodiscard]] bool is_import_candidate(const std::filesystem::path &path)
{
    static const std::set<std::string> raster{".png",  ".jpg", ".jpeg", ".tif",
                                              ".tiff", ".bmp", ".gif",  ".webp"};
    const auto name = path.filename().generic_u8string();
    if (name.empty() || name.front() == u8'.')
    {
        return false;
    }
    return raster.contains(extension_lower(path)) || is_raw_extension(path);
}

[[nodiscard]] Result<std::vector<std::string>>
collect_import_paths(const std::vector<std::string> &inputs, const CancellationToken &cancellation)
{
    std::vector<std::string> files;
    for (const auto &input : inputs)
    {
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        auto location = normalize_local_input(input);
        if (!location)
        {
            continue;
        }
        std::error_code error;
        const std::filesystem::path path(
            std::u8string(location.value().path.begin(), location.value().path.end()));
        if (std::filesystem::is_regular_file(path, error) && !error)
        {
            files.push_back(location.value().path);
            continue;
        }
        if (!std::filesystem::is_directory(path, error) || error)
        {
            continue;
        }
        const auto options = std::filesystem::directory_options::skip_permission_denied;
        for (std::filesystem::recursive_directory_iterator iterator(path, options, error), end;
             iterator != end && !error; iterator.increment(error))
        {
            cancelled = cancellation.check();
            if (!cancelled)
            {
                return cancelled.error();
            }
            if (!iterator->is_regular_file(error) || error)
            {
                continue;
            }
            if (is_import_candidate(iterator->path()))
            {
                const auto utf8 = iterator->path().generic_u8string();
                files.emplace_back(reinterpret_cast<const char *>(utf8.data()), utf8.size());
            }
        }
        if (error)
        {
            return make_error(ErrorCode::kIo, "Unable to enumerate import directory",
                              {{"path", location.value().path}, {"detail", error.message()}});
        }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    LOG_INFO(ravo::logger(), "import enumeration collected {} files from {} inputs", files.size(),
             inputs.size());
    return files;
}

[[nodiscard]] std::string fnv1a64_hex(const std::string_view text)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char character : text)
    {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ULL;
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int index = 15; index >= 0; --index)
    {
        out[static_cast<std::size_t>(index)] = hex[hash & 0xfU];
        hash >>= 4U;
    }
    return out;
}

[[nodiscard]] Recipe identity_recipe_for(const AssetRecord &asset, const std::string &path)
{
    Recipe recipe;
    recipe.asset = {asset.id, path, asset.content_fingerprint};
    recipe.operations.push_back({"ravo.color.input", 1, "color-input-1", true,
                                 input_color_to_parameters(InputColorParams{}), std::nullopt});
    recipe.operations.push_back({"ravo.color.output", 1, "color-output-1", true,
                                 output_color_to_parameters(OutputColorParams{}), std::nullopt});
    return recipe;
}

[[nodiscard]] DevelopParams baseline_develop_for(const AssetRecord &asset)
{
    DevelopParams params;
    params.sigmoid_enabled = is_raw_media_type(asset.media_type);
    return params;
}

[[nodiscard]] Result<Recipe> baseline_recipe_for(const AssetRecord &asset, const std::string &path)
{
    return recipe_from_develop({asset.id, path, asset.content_fingerprint},
                               baseline_develop_for(asset));
}

[[nodiscard]] bool matches_develop_baseline(const AssetRecord &asset, DevelopParams params)
{
    clamp_develop(params);
    return params == baseline_develop_for(asset);
}

[[nodiscard]] std::string parameter_key_part(const ParameterValue &value)
{
    if (const auto *text = std::get_if<std::string>(&value.value))
    {
        return *text;
    }
    if (const auto *number = std::get_if<double>(&value.value))
    {
        return std::to_string(*number);
    }
    if (const auto *integer = std::get_if<std::int64_t>(&value.value))
    {
        return std::to_string(*integer);
    }
    if (const auto *flag = std::get_if<bool>(&value.value))
    {
        return *flag ? "true" : "false";
    }
    if (const auto *array = std::get_if<ParameterValue::Array>(&value.value))
    {
        std::string result = "[";
        for (std::size_t index = 0; index < array->size(); ++index)
        {
            if (index != 0)
            {
                result.push_back(',');
            }
            result += parameter_key_part((*array)[index]);
        }
        result.push_back(']');
        return result;
    }
    return "?";
}

[[nodiscard]] std::string input_color_preprocess_key(const Recipe &recipe)
{
    std::string key;
    for (const auto &operation : recipe.operations)
    {
        if (!operation.enabled)
        {
            continue;
        }
        if (operation.id == kProfileGammaOperationId)
        {
            if (key.empty())
            {
                key = "color";
            }
            key += ":profilegamma";
            for (const auto &[name, value] : operation.parameters)
            {
                key.push_back(':');
                key += name;
                key.push_back('=');
                key += parameter_key_part(value);
            }
            continue;
        }
        if (operation.id != "ravo.color.input")
        {
            continue;
        }
        if (key.empty())
        {
            key = "color";
        }
        for (const auto &[name, value] : operation.parameters)
        {
            key.push_back(':');
            key += name;
            key.push_back('=');
            key += parameter_key_part(value);
        }
        return key;
    }
    return key.empty() ? "color:source:linear_rec709" : key;
}

[[nodiscard]] std::string raw_preprocess_key(const Recipe &recipe)
{
    std::string key = "raw:" + input_color_preprocess_key(recipe);
    bool found_preprocess = false;
    for (const auto &operation : recipe.operations)
    {
        if (!operation.enabled ||
            (operation.id != "ravo.color.temperature" && operation.id != "ravo.raw.hotpixels" &&
             operation.id != "ravo.raw.highlights" && operation.id != "ravo.raw.cacorrect"))
        {
            continue;
        }
        found_preprocess = true;
        key += operation.id == "ravo.color.temperature" ? ":temp" :
               operation.id == "ravo.raw.hotpixels"     ? ":hot" :
               operation.id == "ravo.raw.highlights"    ? ":hl" :
                                                          ":ca";
        static constexpr std::array<std::string_view, 3> highlight_names{"mode", "amount", "clip"};
        static constexpr std::array<std::string_view, 3> hot_pixel_names{"strength", "threshold",
                                                                         "permissive"};
        static constexpr std::array<std::string_view, 2> ca_names{"iterations",
                                                                  "avoid_color_shift"};
        if (operation.id == "ravo.color.temperature")
        {
            for (const auto &[name, value] : operation.parameters)
            {
                key.push_back(':');
                key += name;
                key.push_back('=');
                key += parameter_key_part(value);
            }
            continue;
        }
        if (operation.id == "ravo.raw.cacorrect")
        {
            for (const auto name : ca_names)
            {
                key.push_back(':');
                const auto found = operation.parameters.find(std::string(name));
                key +=
                    found == operation.parameters.end() ? "-" : parameter_key_part(found->second);
            }
            continue;
        }
        const auto &names =
            operation.id == "ravo.raw.hotpixels" ? hot_pixel_names : highlight_names;
        for (const auto name : names)
        {
            key.push_back(':');
            const auto found = operation.parameters.find(std::string(name));
            key += found == operation.parameters.end() ? "-" : parameter_key_part(found->second);
        }
    }
    return found_preprocess ? key : key + ":linear";
}

void disable_raw_preprocess(Recipe &recipe)
{
    for (auto &operation : recipe.operations)
    {
        if (operation.id == "ravo.color.temperature" || operation.id == "ravo.raw.hotpixels" ||
            operation.id == "ravo.raw.highlights" || operation.id == "ravo.raw.cacorrect" ||
            operation.id == kProfileGammaOperationId)
        {
            operation.enabled = false;
        }
    }
}

[[nodiscard]] std::filesystem::path utf8_path(const std::string_view text)
{
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

[[nodiscard]] bool is_disk_full(const std::error_code &error) noexcept
{
    return error == std::errc::no_space_on_device || errno == ENOSPC;
}

[[nodiscard]] TaskError export_io_error(std::string message, const std::string_view path,
                                        const std::error_code &error)
{
    std::map<std::string, std::string, std::less<>> context{{"path", std::string(path)}};
    if (error)
    {
        context.emplace("detail", error.message());
    }
    if (is_disk_full(error))
    {
        context.emplace("reason", "disk_full");
    }
    return make_error(ErrorCode::kIo, std::move(message), std::move(context));
}

[[nodiscard]] Result<void> write_bytes_atomically(const std::string_view dest_utf8,
                                                  const std::vector<std::uint8_t> &bytes,
                                                  const CancellationToken &cancellation)
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    const auto dest = utf8_path(dest_utf8);
    std::error_code error;
    if (std::filesystem::exists(dest, error))
    {
        return make_error(ErrorCode::kConflict, "Export output already exists",
                          {{"path", std::string(dest_utf8)}});
    }
    if (error)
    {
        return export_io_error("Unable to inspect export output path", dest_utf8, error);
    }
    const auto parent = dest.parent_path();
    if (!parent.empty() && !std::filesystem::is_directory(parent, error))
    {
        return make_error(ErrorCode::kIo, "Export directory does not exist",
                          {{"path", std::string(dest_utf8)}});
    }
    auto temporary = dest;
    temporary += ".ravo-export-tmp";
    std::filesystem::remove(temporary, error);
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output)
        {
            std::filesystem::remove(temporary, error);
            return export_io_error("Unable to open temporary export file", dest_utf8,
                                   std::error_code(errno, std::generic_category()));
        }
        constexpr std::size_t kChunk = 64U * 1024U;
        std::size_t offset = 0;
        while (offset < bytes.size())
        {
            cancelled = cancellation.check();
            if (!cancelled)
            {
                output.close();
                std::filesystem::remove(temporary, error);
                return cancelled.error();
            }
            const auto remaining = bytes.size() - offset;
            const auto step = remaining < kChunk ? remaining : kChunk;
            output.write(reinterpret_cast<const char *>(bytes.data() + offset),
                         static_cast<std::streamsize>(step));
            if (!output)
            {
                output.close();
                std::filesystem::remove(temporary, error);
                return export_io_error("Unable to write export file", dest_utf8,
                                       std::error_code(errno, std::generic_category()));
            }
            offset += step;
        }
        output.close();
        if (!output)
        {
            std::filesystem::remove(temporary, error);
            return export_io_error("Unable to finish export file", dest_utf8,
                                   std::error_code(errno, std::generic_category()));
        }
    }
    std::filesystem::rename(temporary, dest, error);
    if (error)
    {
        std::filesystem::remove(temporary, error);
        if (std::filesystem::exists(dest))
        {
            return make_error(ErrorCode::kConflict, "Export output already exists",
                              {{"path", std::string(dest_utf8)}});
        }
        return export_io_error("Unable to commit export file", dest_utf8, error);
    }
    return {};
}

[[nodiscard]] Result<std::uint64_t> copy_file_atomically(const std::string_view source_utf8,
                                                         const std::string_view dest_utf8,
                                                         const CancellationToken &cancellation)
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    const auto source = utf8_path(source_utf8);
    const auto dest = utf8_path(dest_utf8);
    std::error_code error;
    if (std::filesystem::exists(dest, error))
    {
        return make_error(ErrorCode::kConflict, "Export output already exists",
                          {{"path", std::string(dest_utf8)}});
    }
    if (error)
    {
        return export_io_error("Unable to inspect export output path", dest_utf8, error);
    }
    std::ifstream input(source, std::ios::binary);
    if (!input)
    {
        return make_error(ErrorCode::kNotFound, "Original file is missing",
                          {{"path", std::string(source_utf8)}});
    }
    std::vector<std::uint8_t> bytes;
    constexpr std::size_t kChunk = 64U * 1024U;
    std::vector<char> buffer(kChunk);
    while (input)
    {
        cancelled = cancellation.check();
        if (!cancelled)
        {
            return cancelled.error();
        }
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto read = input.gcount();
        if (read > 0)
        {
            const auto *data = reinterpret_cast<const std::uint8_t *>(buffer.data());
            bytes.insert(bytes.end(), data, data + static_cast<std::size_t>(read));
        }
    }
    if (!input.eof())
    {
        return export_io_error("Unable to read original file", source_utf8,
                               std::error_code(errno, std::generic_category()));
    }
    auto written = write_bytes_atomically(dest_utf8, bytes, cancellation);
    if (!written)
    {
        return written.error();
    }
    return static_cast<std::uint64_t>(bytes.size());
}

} // namespace ravo
