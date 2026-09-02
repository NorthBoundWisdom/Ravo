#include "catalog_internal.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <map>
#include <set>
#include <system_error>
#include <utility>

#include "ravo/domain/uri.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/dehaze.h"
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

[[nodiscard]] bool is_jpeg_extension(const std::filesystem::path &path)
{
    const auto extension = extension_lower(path);
    return extension == ".jpg" || extension == ".jpeg";
}

[[nodiscard]] std::string path_utf8(const std::filesystem::path &path)
{
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}

[[nodiscard]] Result<std::optional<std::string>> adjacent_jpeg(const std::string_view source)
{
    const auto path = utf8_path(source);
    std::vector<std::filesystem::path> found;
    for (const auto &extension : {u8".jpg", u8".jpeg", u8".JPG", u8".JPEG"})
    {
        auto candidate = path;
        candidate.replace_extension(extension);
        std::error_code error;
        if (std::filesystem::is_regular_file(candidate, error) && !error)
        {
            if (std::none_of(found.begin(), found.end(),
                             [&](const auto &existing)
                             {
                                 std::error_code equivalent_error;
                                 return std::filesystem::equivalent(existing, candidate,
                                                                    equivalent_error) &&
                                        !equivalent_error;
                             }))
                found.push_back(std::move(candidate));
        }
        else if (error && error != std::errc::no_such_file_or_directory)
        {
            return make_error(ErrorCode::kIo, "Unable to inspect RAW JPEG companion",
                              {{"path", path_utf8(candidate)},
                               {"reason", "import_jpeg_companion_inspect_failed"},
                               {"detail", error.message()}});
        }
    }
    if (found.size() > 1U)
        return make_error(ErrorCode::kConflict, "Multiple JPEG companions match one RAW file",
                          {{"source", std::string(source)},
                           {"reason", "import_jpeg_companion_ambiguous"}});
    return found.empty() ? std::optional<std::string>{} :
                           std::optional<std::string>{path_utf8(found.front())};
}

void drop_raw_companion_jpegs(std::vector<std::string> &files)
{
    struct Stem
    {
        bool has_raw = false;
        std::vector<std::size_t> jpeg_indexes;
    };
    std::map<std::string, Stem> stems;
    for (std::size_t index = 0; index < files.size(); ++index)
    {
        const auto path = utf8_path(files[index]);
        auto key = path_utf8(path.parent_path()) + '\n' + lower_ascii(path_utf8(path.stem()));
        if (is_raw_extension(path))
            stems[key].has_raw = true;
        else if (is_jpeg_extension(path))
            stems[key].jpeg_indexes.push_back(index);
    }
    std::vector<char> drop(files.size(), 0);
    std::size_t dropped = 0;
    for (const auto &[ignored, stem] : stems)
    {
        static_cast<void>(ignored);
        if (!stem.has_raw)
            continue;
        for (const auto jpeg_index : stem.jpeg_indexes)
        {
            drop[jpeg_index] = 1;
            ++dropped;
        }
    }
    if (dropped == 0)
        return;
    std::vector<std::string> kept;
    kept.reserve(files.size() - dropped);
    for (std::size_t index = 0; index < files.size(); ++index)
        if (drop[index] == 0)
            kept.push_back(std::move(files[index]));
    files.swap(kept);
    LOG_INFO(ravo::logger(), "import enumeration omitted {} RAW JPEG companions", dropped);
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
collect_import_paths(const std::vector<std::string> &inputs, const CancellationToken &cancellation,
                     const bool recursive)
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
            if (files.size() > kImportBatchMaximumAssets)
                return make_error(ErrorCode::kValidation, "Import enumeration exceeds its bound",
                                  {{"maximum_assets", std::to_string(kImportBatchMaximumAssets)},
                                   {"reason", "import_asset_count_exceeded"}});
            continue;
        }
        if (!std::filesystem::is_directory(path, error) || error)
        {
            continue;
        }
        const auto options = std::filesystem::directory_options::skip_permission_denied;
        const auto append_candidate = [&](const std::filesystem::path &candidate) -> Result<void>
        {
            auto still_active = cancellation.check();
            if (!still_active)
                return still_active.error();
            if (!is_import_candidate(candidate))
                return {};
            const auto utf8 = candidate.generic_u8string();
            files.emplace_back(reinterpret_cast<const char *>(utf8.data()), utf8.size());
            if (files.size() > kImportBatchMaximumAssets)
                return make_error(ErrorCode::kValidation, "Import enumeration exceeds its bound",
                                  {{"maximum_assets", std::to_string(kImportBatchMaximumAssets)},
                                   {"reason", "import_asset_count_exceeded"}});
            return {};
        };
        if (recursive)
        {
            for (std::filesystem::recursive_directory_iterator iterator(path, options, error), end;
                 iterator != end && !error; iterator.increment(error))
            {
                if (!iterator->is_regular_file(error) || error)
                    continue;
                auto appended = append_candidate(iterator->path());
                if (!appended)
                    return appended.error();
            }
        }
        else
        {
            for (std::filesystem::directory_iterator iterator(path, options, error), end;
                 iterator != end && !error; iterator.increment(error))
            {
                if (!iterator->is_regular_file(error) || error)
                    continue;
                auto appended = append_candidate(iterator->path());
                if (!appended)
                    return appended.error();
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
    drop_raw_companion_jpegs(files);
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
    // RAW import colour calibration: as-shot WB (default temperature), the
    // file's camera matrix via input profile `source`, and Sigmoid. Later
    // Develop edits stack on this baseline. Adobe DCP is not used.
    params.sigmoid_enabled = is_raw_media_type(asset.media_type);
    return params;
}

[[nodiscard]] Result<Recipe> baseline_recipe_for(const AssetRecord &asset, const std::string &path)
{
    auto recipe = recipe_from_develop({asset.id, path, asset.content_fingerprint},
                                      baseline_develop_for(asset));
    if (!recipe)
        return recipe.error();
    if (!is_raw_media_type(asset.media_type))
        return recipe;
    auto &operations = recipe.value().operations;
    const bool has_temperature =
        std::any_of(operations.begin(), operations.end(),
                    [](const OperationInstance &operation)
                    { return operation.id == "ravo.color.temperature"; });
    if (has_temperature)
        return recipe;
    OperationInstance temperature{"ravo.color.temperature",
                                  1,
                                  "temperature-1",
                                  true,
                                  temperature_to_parameters(TemperatureParams{}),
                                  std::nullopt};
    const auto input = std::find_if(operations.begin(), operations.end(),
                                    [](const OperationInstance &operation)
                                    { return operation.id == "ravo.color.input"; });
    operations.insert(input, std::move(temperature));
    return recipe;
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
             operation.id != "ravo.raw.highlights" && operation.id != "ravo.raw.cacorrect" &&
             operation.id != "ravo.raw.denoise" && operation.id != kDehazeOperationId))
        {
            continue;
        }
        found_preprocess = true;
        key += operation.id == "ravo.color.temperature" ? ":temp" :
               operation.id == "ravo.raw.hotpixels"     ? ":hot" :
               operation.id == "ravo.raw.highlights"    ? ":hl" :
               operation.id == "ravo.raw.denoise"       ? ":rdn" :
               operation.id == kDehazeOperationId       ? ":dehaze" :
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
        if (operation.id == "ravo.raw.denoise")
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
        if (operation.id == kDehazeOperationId)
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
            operation.id == "ravo.raw.denoise" || operation.id == kProfileGammaOperationId ||
            operation.id == kDehazeOperationId)
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
    return error == std::errc::no_space_on_device;
}

} // namespace ravo
