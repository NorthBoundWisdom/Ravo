#include "ravo/services/catalog_service.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

#include "catalog_internal.h"
#include "ravo/domain/uri.h"

namespace ravo
{
namespace
{

struct PlannedImport
{
    ImportCandidate candidate;
    std::string import_path;
    std::optional<std::string> source_sidecar;
    std::optional<FileIdentity> source_sidecar_identity;
    std::optional<std::string> destination_sidecar;
    std::optional<std::string> source_jpeg;
    std::optional<FileIdentity> source_jpeg_identity;
    std::optional<std::string> destination_jpeg;
    std::optional<std::string> second_copy_path;
    std::optional<std::string> second_copy_sidecar;
    std::optional<std::string> second_copy_jpeg;
};

[[nodiscard]] std::string path_text(const std::filesystem::path &path)
{
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}

[[nodiscard]] std::string date_directory(const ImportCandidate &candidate)
{
    if (candidate.captured_date_path)
        return *candidate.captured_date_path;
    using namespace std::chrono;
    const auto milliseconds =
        candidate.captured_unix_s ?
            duration_cast<std::chrono::milliseconds>(seconds(*candidate.captured_unix_s)) :
            std::chrono::milliseconds(candidate.mtime_unix_ms);
    const year_month_day date{floor<days>(sys_time<std::chrono::milliseconds>{milliseconds})};
    std::ostringstream stream;
    stream << std::setfill('0') << std::setw(4) << static_cast<int>(date.year()) << '/'
           << std::setw(2) << static_cast<unsigned>(date.month()) << '/' << std::setw(2)
           << static_cast<unsigned>(date.day());
    return stream.str();
}

[[nodiscard]] Result<std::optional<std::string>> adjacent_xmp(const std::string_view source)
{
    const auto path = utf8_path(source);
    std::vector<std::filesystem::path> found;
    for (const auto &extension : {u8".xmp", u8".XMP"})
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
            return make_error(ErrorCode::kIo, "Unable to inspect import sidecar",
                              {{"path", path_text(candidate)},
                               {"reason", "import_sidecar_inspect_failed"},
                               {"detail", error.message()}});
        }
    }
    if (found.size() > 1U)
        return make_error(
            ErrorCode::kConflict, "Multiple XMP sidecars match one photo",
            {{"source", std::string(source)}, {"reason", "import_sidecar_ambiguous"}});
    return found.empty() ? std::optional<std::string>{} :
                           std::optional<std::string>{path_text(found.front())};
}

[[nodiscard]] bool is_safe_relative_path(const std::filesystem::path &path)
{
    if (path.empty() || path.is_absolute())
        return false;
    return std::ranges::none_of(path, [](const auto &component) { return component == u8".."; });
}

[[nodiscard]] std::string portable_path_key(std::string value)
{
    std::ranges::transform(value, value.begin(),
                           [](const unsigned char character)
                           {
                               return character >= 'A' && character <= 'Z' ?
                                          static_cast<char>(character - 'A' + 'a') :
                                          static_cast<char>(character);
                           });
    return value;
}

void annotate_cleanup_failure(TaskError &primary, const std::string_view path,
                              const std::error_code &error)
{
    if (!primary.context.contains("cleanup_path"))
    {
        primary.context.emplace("cleanup_path", std::string(path));
        primary.context.emplace("cleanup_detail", error.message());
    }
    primary.context.insert_or_assign("cleanup_failed", "true");
}

void remove_owned_files(std::vector<std::string> &paths, TaskError &primary)
{
    for (auto iterator = paths.rbegin(); iterator != paths.rend(); ++iterator)
    {
        std::error_code error;
        const bool removed = std::filesystem::remove(utf8_path(*iterator), error);
        if (error)
        {
            annotate_cleanup_failure(primary, *iterator, error);
            continue;
        }
        if (!removed)
        {
            const bool still_exists = std::filesystem::exists(utf8_path(*iterator), error);
            if (error || still_exists)
                annotate_cleanup_failure(primary, *iterator,
                                         error ? error : std::make_error_code(std::errc::io_error));
        }
    }
    paths.clear();
}

} // namespace

Result<ImportCandidate>
CatalogService::inspect_import_candidate(const std::string_view path,
                                         const std::string_view source_root,
                                         const CancellationToken &cancellation) const
{
    auto active = cancellation.check();
    if (!active)
        return active.error();
    if (repository_ == nullptr || raster_ == nullptr || engine_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto location = normalize_local_input(path);
    if (!location)
        return location.error();
    auto identity = read_file_identity(location.value().path);
    if (!identity)
        return identity.error();

    ImportCandidate candidate;
    candidate.source_path = location.value().path;
    candidate.display_name = path_text(utf8_path(candidate.source_path).filename());
    candidate.size_bytes = identity.value().size_bytes;
    candidate.mtime_unix_ms = identity.value().mtime_unix_ms;
    if (!source_root.empty())
    {
        std::error_code relative_error;
        const auto relative = std::filesystem::relative(utf8_path(candidate.source_path),
                                                        utf8_path(source_root), relative_error);
        if (!relative_error && is_safe_relative_path(relative))
            candidate.relative_path = path_text(relative);
    }
    if (candidate.relative_path.empty())
        candidate.relative_path = candidate.display_name;

    auto existing = repository_->find_asset_by_uri(location.value().uri);
    if (!existing)
        return existing.error();
    candidate.duplicate = existing.value().has_value();

    const auto source_path = utf8_path(candidate.source_path);
    if (is_raw_extension(source_path))
    {
        auto inspected = engine_->inspect_with_embedded_preview(candidate.source_path,
                                                                kThumbnailMaxEdge, cancellation);
        if (!inspected)
        {
            candidate.supported = false;
            candidate.error = inspected.error();
            return candidate;
        }
        candidate.media_type = std::string(kMediaTypeRaw);
        candidate.width = inspected.value().inspection.width;
        candidate.height = inspected.value().inspection.height;
        candidate.captured_unix_s = inspected.value().inspection.captured_unix_s;
        auto companion = adjacent_jpeg(candidate.source_path);
        if (!companion)
        {
            candidate.supported = false;
            candidate.error = companion.error();
            return candidate;
        }
        return candidate;
    }
    auto raster = raster_->probe(candidate.source_path);
    if (!raster)
    {
        candidate.supported = false;
        candidate.error = raster.error();
        return candidate;
    }
    candidate.media_type = raster.value().media_type;
    candidate.width = raster.value().width;
    candidate.height = raster.value().height;
    auto metadata = engine_->read_embedded_capture_metadata(candidate.source_path, cancellation);
    if (metadata && metadata.value().captured_datetime)
    {
        const auto &local = metadata.value().captured_datetime->local_exif;
        if (local.size() >= 10U && local[4] == ':' && local[7] == ':')
            candidate.captured_date_path =
                local.substr(0U, 4U) + '/' + local.substr(5U, 2U) + '/' + local.substr(8U, 2U);
    }
    return candidate;
}

Result<RasterBuffer>
CatalogService::decode_import_candidate_thumbnail(const std::string_view path,
                                                  const CancellationToken &cancellation) const
{
    if (raster_ == nullptr || engine_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto location = normalize_local_input(path);
    if (!location)
        return location.error();
    const auto raster_from_decoded = [](DecodedRaster value) -> RasterBuffer
    {
        RasterBuffer result;
        result.width = value.width;
        result.height = value.height;
        result.source_width = value.source_width;
        result.source_height = value.source_height;
        result.srgb = std::move(value.rgb);
        result.color_profile = std::move(value.color_profile);
        return result;
    };
    if (!is_raw_extension(utf8_path(location.value().path)))
    {
        auto decoded = raster_->decode(location.value().path, kThumbnailMaxEdge, cancellation);
        if (!decoded)
            return decoded.error();
        return raster_from_decoded(std::move(decoded).value());
    }
    auto companion = adjacent_jpeg(location.value().path);
    if (!companion)
        return companion.error();
    if (companion.value())
    {
        auto decoded = raster_->decode(*companion.value(), kThumbnailMaxEdge, cancellation);
        if (decoded)
            return raster_from_decoded(std::move(decoded).value());
    }
    auto inspected = engine_->inspect_with_embedded_preview(location.value().path,
                                                            kThumbnailMaxEdge, cancellation);
    if (!inspected)
        return inspected.error();
    if (!inspected.value().embedded_preview)
    {
        auto identity = read_file_identity(location.value().path);
        if (!identity)
            return identity.error();
        AssetRecord asset;
        asset.id = "import-candidate";
        asset.normalized_uri = location.value().uri;
        asset.media_type = std::string(kMediaTypeRaw);
        asset.width = inspected.value().inspection.width;
        asset.height = inspected.value().inspection.height;
        asset.content_fingerprint = make_content_fingerprint(identity.value());
        auto recipe = baseline_recipe_for(asset, location.value().path);
        if (!recipe)
            return recipe.error();
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        fit_within_max_edge(*asset.width, *asset.height, kThumbnailMaxEdge, width, height);
        RenderRequest request;
        request.asset = {asset.id, location.value().path, asset.content_fingerprint};
        request.recipe = std::move(recipe).value();
        request.output_width = width;
        request.output_height = height;
        request.cancellation = cancellation;
        auto rendered = engine_->render_to_image(request);
        if (!rendered)
            return rendered.error();
        auto value = std::move(rendered).value();
        RasterBuffer result;
        result.width = value.width;
        result.height = value.height;
        result.source_width = *asset.width;
        result.source_height = *asset.height;
        result.srgb = std::move(value.rgb);
        result.color_profile = std::move(value.color_profile);
        return result;
    }
    auto decoded =
        raster_->decode_memory(inspected.value().embedded_preview->bytes, kThumbnailMaxEdge,
                               cancellation, inspected.value().embedded_preview->rotate_quarters);
    if (!decoded)
        return decoded.error();
    return raster_from_decoded(std::move(decoded).value());
}

Result<ImportBatchResult> CatalogService::execute_import(
    const ImportRequest &request,
    const std::function<void(std::size_t, std::size_t, const ImportItemResult *)> &progress)
{
    if (request.inputs.empty())
        return make_error(ErrorCode::kInvalidArgument, "Import requires at least one input");
    if (request.mode == ImportTransferMode::kAdd &&
        (!request.destination_directory.empty() || !request.filename_template.empty() ||
         !request.second_copy_directory.empty()))
        return make_error(ErrorCode::kInvalidArgument,
                          "Add import must not specify destinations or a filename template",
                          {{"reason", "add_import_transfer_options_unsupported"}});
    if (request.mode != ImportTransferMode::kAdd && request.destination_directory.empty())
        return make_error(ErrorCode::kInvalidArgument,
                          "Copy and move import require a destination directory",
                          {{"reason", "missing_import_destination"}});
    if (!request.filename_template.empty())
    {
        auto valid_template = expand_import_filename_template(request.filename_template, "sample",
                                                              "19700101", 1U, ".raw");
        if (!valid_template)
            return valid_template.error();
    }

    auto paths = collect_import_paths(request.inputs, request.cancellation, request.recursive);
    if (!paths)
        return paths.error();
    std::filesystem::path destination_root;
    std::optional<std::filesystem::path> second_copy_root;
    const auto existing_directory =
        [](const std::string_view text,
           const std::string_view reason) -> Result<std::filesystem::path>
    {
        auto location = normalize_local_input(text);
        if (!location)
            return location.error();
        const auto path = utf8_path(location.value().path);
        std::error_code error;
        if (!std::filesystem::is_directory(path, error) || error)
            return make_error(ErrorCode::kInvalidArgument,
                              "Import destination is not an existing directory",
                              {{"path", location.value().path},
                               {"reason", std::string(reason)},
                               {"detail", error.message()}});
        return path;
    };
    if (request.mode != ImportTransferMode::kAdd)
    {
        auto destination =
            existing_directory(request.destination_directory, "invalid_import_destination");
        if (!destination)
            return destination.error();
        destination_root = std::move(destination).value();
        if (!request.second_copy_directory.empty())
        {
            auto second = existing_directory(request.second_copy_directory,
                                             "invalid_import_second_copy_destination");
            if (!second)
                return second.error();
            if (portable_path_key(path_text(destination_root)) ==
                portable_path_key(path_text(second.value())))
                return make_error(
                    ErrorCode::kConflict,
                    "Primary and second-copy import roots must be different directories",
                    {{"destination", path_text(destination_root)},
                     {"second_copy_destination", path_text(second.value())},
                     {"reason", "import_copy_roots_conflict"}});
            second_copy_root = std::move(second).value();
        }
    }

    std::string source_root = request.source_root.value_or(request.inputs.front());
    auto normalized_source_root = normalize_local_input(source_root);
    if (normalized_source_root)
        source_root = normalized_source_root.value().path;
    std::error_code root_error;
    if (std::filesystem::is_regular_file(utf8_path(source_root), root_error) && !root_error)
        source_root = path_text(utf8_path(source_root).parent_path());
    std::vector<PlannedImport> plan;
    plan.reserve(paths.value().size());
    std::set<std::string, std::less<>> outputs;
    const auto preflight_output = [&](const std::string_view source,
                                      const std::filesystem::path &output) -> Result<std::string>
    {
        auto normalized_source = normalize_local_input(source);
        if (!normalized_source)
            return normalized_source.error();
        auto normalized_output = normalize_local_input(path_text(output));
        if (!normalized_output)
            return normalized_output.error();
        const auto output_key = portable_path_key(normalized_output.value().path);
        if (output_key == portable_path_key(normalized_source.value().path))
            return make_error(ErrorCode::kConflict, "Import output aliases its source",
                              {{"source", normalized_source.value().path},
                               {"output", normalized_output.value().path},
                               {"reason", "import_output_aliases_source"}});
        if (!outputs.insert(output_key).second)
            return make_error(ErrorCode::kConflict,
                              "Import destinations contain duplicate portable planned paths",
                              {{"output", normalized_output.value().path},
                               {"reason", "duplicate_import_output"}});

        std::error_code target_error;
        const auto status = std::filesystem::symlink_status(
            utf8_path(normalized_output.value().path), target_error);
        if (!target_error && status.type() != std::filesystem::file_type::not_found)
            return make_error(ErrorCode::kConflict, "Import destination already exists",
                              {{"output", normalized_output.value().path},
                               {"reason", "import_destination_conflict"}});
        if (target_error && target_error != std::errc::no_such_file_or_directory)
            return make_error(ErrorCode::kIo, "Unable to inspect import destination",
                              {{"output", normalized_output.value().path},
                               {"detail", target_error.message()},
                               {"reason", "import_destination_inspect_failed"}});
        auto existing = repository_->find_asset_by_uri(normalized_output.value().uri);
        if (!existing)
            return existing.error();
        if (existing.value())
            return make_error(ErrorCode::kConflict,
                              "Import destination is already present in the catalog",
                              {{"output", normalized_output.value().path},
                               {"reason", "import_destination_catalog_conflict"}});
        return normalized_output.value().path;
    };
    for (std::size_t index = 0U; index < paths.value().size(); ++index)
    {
        const auto &source = paths.value()[index];
        auto candidate = inspect_import_candidate(source, source_root, request.cancellation);
        if (!candidate)
            return candidate.error();
        PlannedImport item;
        item.candidate = std::move(candidate).value();
        item.import_path = item.candidate.source_path;
        if (request.include_xmp_sidecars)
        {
            auto sidecar = adjacent_xmp(item.candidate.source_path);
            if (!sidecar)
                return sidecar.error();
            item.source_sidecar = std::move(sidecar).value();
            if (item.source_sidecar)
            {
                auto identity = read_file_identity(*item.source_sidecar);
                if (!identity)
                    return identity.error();
                item.source_sidecar_identity = std::move(identity).value();
            }
        }
        if (is_raw_extension(utf8_path(item.candidate.source_path)))
        {
            auto jpeg = adjacent_jpeg(item.candidate.source_path);
            if (!jpeg)
                return jpeg.error();
            item.source_jpeg = std::move(jpeg).value();
            if (item.source_jpeg)
            {
                auto identity = read_file_identity(*item.source_jpeg);
                if (!identity)
                    return identity.error();
                item.source_jpeg_identity = std::move(identity).value();
            }
        }
        if (request.mode != ImportTransferMode::kAdd)
        {
            const auto source_path = utf8_path(item.candidate.source_path);
            std::string filename = item.candidate.display_name;
            if (!request.filename_template.empty())
            {
                std::string date = date_directory(item.candidate);
                std::erase(date, '/');
                auto expanded = expand_import_filename_template(
                    request.filename_template, path_text(source_path.stem()), date, index + 1U,
                    path_text(source_path.extension()));
                if (!expanded)
                    return expanded.error();
                filename = std::move(expanded).value();
            }
            std::filesystem::path relative;
            if (request.organization == ImportOrganization::kSingleFolder)
                relative = utf8_path(filename);
            else if (request.organization == ImportOrganization::kCaptureDate)
                relative = utf8_path(date_directory(item.candidate)) / utf8_path(filename);
            else
            {
                const auto root_name = utf8_path(source_root).filename();
                if (root_name.empty())
                    return make_error(ErrorCode::kValidation,
                                      "Preserved hierarchy requires a named source root",
                                      {{"reason", "import_source_root_unnamed"}});
                const auto candidate_relative = utf8_path(item.candidate.relative_path);
                if (!is_safe_relative_path(candidate_relative))
                    return make_error(ErrorCode::kValidation,
                                      "Import source relative path is unsafe",
                                      {{"source", item.candidate.source_path},
                                       {"reason", "unsafe_import_relative_path"}});
                relative = root_name / candidate_relative.parent_path() / utf8_path(filename);
            }
            auto primary =
                preflight_output(item.candidate.source_path, destination_root / relative);
            if (!primary)
                return primary.error();
            item.import_path = std::move(primary).value();
            if (item.source_sidecar)
            {
                auto sidecar_output = utf8_path(item.import_path);
                sidecar_output.replace_extension(utf8_path(*item.source_sidecar).extension());
                auto sidecar = preflight_output(*item.source_sidecar, sidecar_output);
                if (!sidecar)
                    return sidecar.error();
                item.destination_sidecar = std::move(sidecar).value();
            }
            if (item.source_jpeg)
            {
                auto jpeg_output = utf8_path(item.import_path);
                jpeg_output.replace_extension(utf8_path(*item.source_jpeg).extension());
                auto jpeg = preflight_output(*item.source_jpeg, jpeg_output);
                if (!jpeg)
                    return jpeg.error();
                item.destination_jpeg = std::move(jpeg).value();
            }
            if (second_copy_root)
            {
                auto second =
                    preflight_output(item.candidate.source_path, *second_copy_root / relative);
                if (!second)
                    return second.error();
                item.second_copy_path = std::move(second).value();
                if (item.source_sidecar)
                {
                    auto second_sidecar = utf8_path(*item.second_copy_path);
                    second_sidecar.replace_extension(utf8_path(*item.source_sidecar).extension());
                    auto sidecar = preflight_output(*item.source_sidecar, second_sidecar);
                    if (!sidecar)
                        return sidecar.error();
                    item.second_copy_sidecar = std::move(sidecar).value();
                }
                if (item.source_jpeg)
                {
                    auto second_jpeg = utf8_path(*item.second_copy_path);
                    second_jpeg.replace_extension(utf8_path(*item.source_jpeg).extension());
                    auto jpeg = preflight_output(*item.source_jpeg, second_jpeg);
                    if (!jpeg)
                        return jpeg.error();
                    item.second_copy_jpeg = std::move(jpeg).value();
                }
            }
        }
        plan.push_back(std::move(item));
    }

    ImportBatchResult batch;
    batch.mode = request.mode;
    batch.preview = request.preview;
    batch.items.reserve(plan.size());
    for (std::size_t index = 0; index < plan.size(); ++index)
    {
        auto active = request.cancellation.check();
        if (!active)
            break;
        auto &planned = plan[index];
        std::vector<std::string> owned_outputs;
        std::optional<TaskError> transfer_error;
        const auto ensure_parent = [&](const std::string_view output) -> Result<void>
        {
            std::error_code directory_error;
            std::filesystem::create_directories(utf8_path(output).parent_path(), directory_error);
            if (directory_error)
                return make_error(ErrorCode::kIo, "Unable to create import destination directory",
                                  {{"output", std::string(output)},
                                   {"detail", directory_error.message()},
                                   {"reason", "import_destination_create_failed"}});
            return {};
        };
        const auto publish_copy = [&](const std::string_view source,
                                      const std::string_view output) -> Result<void>
        {
            auto parent = ensure_parent(output);
            if (!parent)
                return parent.error();
            auto copied = copy_file_atomically(source, output, request.cancellation);
            if (!copied)
                return copied.error();
            owned_outputs.emplace_back(output);
            return {};
        };
        const auto checkpoint = [&](const std::string_view name,
                                    const std::string_view path) -> Result<void>
        {
            if (!testing_import_checkpoint_)
                return {};
            auto checked = testing_import_checkpoint_(name, path);
            if (!checked)
            {
                auto error = std::move(checked).error();
                error.context.insert_or_assign("checkpoint", std::string(name));
                error.context.insert_or_assign("path", std::string(path));
                return error;
            }
            return {};
        };
        if (request.mode != ImportTransferMode::kAdd)
        {
            auto copied = publish_copy(planned.candidate.source_path, planned.import_path);
            if (!copied)
                transfer_error = copied.error();
            if (!transfer_error && planned.source_sidecar)
            {
                auto copied_sidecar =
                    publish_copy(*planned.source_sidecar, *planned.destination_sidecar);
                if (!copied_sidecar)
                    transfer_error = copied_sidecar.error();
            }
            if (!transfer_error && planned.source_jpeg)
            {
                auto copied_jpeg = publish_copy(*planned.source_jpeg, *planned.destination_jpeg);
                if (!copied_jpeg)
                    transfer_error = copied_jpeg.error();
            }
            if (!transfer_error && planned.second_copy_path)
            {
                auto copied_second =
                    publish_copy(planned.candidate.source_path, *planned.second_copy_path);
                if (!copied_second)
                    transfer_error = copied_second.error();
            }
            if (!transfer_error && planned.second_copy_sidecar)
            {
                auto copied_second_sidecar =
                    publish_copy(*planned.source_sidecar, *planned.second_copy_sidecar);
                if (!copied_second_sidecar)
                    transfer_error = copied_second_sidecar.error();
            }
            if (!transfer_error && planned.second_copy_jpeg)
            {
                auto copied_second_jpeg =
                    publish_copy(*planned.source_jpeg, *planned.second_copy_jpeg);
                if (!copied_second_jpeg)
                    transfer_error = copied_second_jpeg.error();
            }
            if (!transfer_error && planned.second_copy_path)
            {
                auto reached = checkpoint("before_copy_verification", *planned.second_copy_path);
                if (!reached)
                    transfer_error = reached.error();
            }
            if (!transfer_error && planned.second_copy_path)
            {
                for (const auto &pair :
                     std::array<std::pair<std::string_view, std::string_view>, 2U>{
                         std::pair<std::string_view, std::string_view>{
                             planned.candidate.source_path, planned.import_path},
                         {planned.candidate.source_path, *planned.second_copy_path}})
                {
                    auto verified =
                        verify_files_identical(pair.first, pair.second, request.cancellation);
                    if (!verified)
                    {
                        transfer_error = verified.error();
                        break;
                    }
                }
            }
            if (!transfer_error && planned.second_copy_sidecar)
            {
                for (const auto &output : std::array<std::string_view, 2U>{
                         *planned.destination_sidecar, *planned.second_copy_sidecar})
                {
                    auto verified = verify_files_identical(*planned.source_sidecar, output,
                                                           request.cancellation);
                    if (!verified)
                    {
                        transfer_error = verified.error();
                        break;
                    }
                }
            }
            if (!transfer_error && planned.second_copy_jpeg)
            {
                for (const auto &output : std::array<std::string_view, 2U>{
                         *planned.destination_jpeg, *planned.second_copy_jpeg})
                {
                    auto verified =
                        verify_files_identical(*planned.source_jpeg, output, request.cancellation);
                    if (!verified)
                    {
                        transfer_error = verified.error();
                        break;
                    }
                }
            }
        }
        ImportItemResult result;
        bool stop_after_result = false;
        if (transfer_error)
        {
            auto error = std::move(*transfer_error);
            remove_owned_files(owned_outputs, error);
            result = failed_item(planned.candidate.source_path, std::move(error));
            stop_after_result = true;
        }
        else
        {
            auto imported = import_one(planned.import_path, request.cancellation, request.preview,
                                       request.defer_previews);
            result = imported ? std::move(imported).value() :
                                failed_item(planned.candidate.source_path, imported.error());
        }
        result.input_path = planned.candidate.source_path;
        if (request.mode != ImportTransferMode::kAdd)
        {
            result.destination_path = planned.import_path;
            result.sidecar_destination_path = planned.destination_sidecar;
            result.jpeg_companion_destination_path = planned.destination_jpeg;
            result.second_copy_destination_path = planned.second_copy_path;
            result.second_copy_sidecar_destination_path = planned.second_copy_sidecar;
            result.second_copy_jpeg_companion_destination_path = planned.second_copy_jpeg;
            result.copies_verified = planned.second_copy_path && !transfer_error;
            if (result.status == ImportItemStatus::kFailed ||
                result.status == ImportItemStatus::kUnsupported)
            {
                if (!owned_outputs.empty())
                {
                    if (!result.error)
                        result.error =
                            make_error(ErrorCode::kIo, "Import failed before catalog publication",
                                       {{"reason", "import_prepublication_failed"}});
                    remove_owned_files(owned_outputs, *result.error);
                }
                result.copies_verified = false;
            }
            else if (result.status == ImportItemStatus::kDuplicate)
            {
                result.error =
                    make_error(ErrorCode::kConflict,
                               "Import destination was cataloged concurrently after publication",
                               {{"output", planned.import_path},
                                {"reason", "import_destination_catalog_race"}});
            }
            else
            {
                owned_outputs.clear();
            }
        }
        if (request.mode == ImportTransferMode::kMove &&
            result.status == ImportItemStatus::kImported)
        {
            std::error_code remove_error;
            auto current_identity = read_file_identity(planned.candidate.source_path);
            Result<FileIdentity> current_sidecar_identity =
                planned.source_sidecar ? read_file_identity(*planned.source_sidecar) :
                                         Result<FileIdentity>{FileIdentity{}};
            Result<FileIdentity> current_jpeg_identity =
                planned.source_jpeg ? read_file_identity(*planned.source_jpeg) :
                                      Result<FileIdentity>{FileIdentity{}};
            if (!current_identity ||
                current_identity.value().size_bytes != planned.candidate.size_bytes ||
                current_identity.value().mtime_unix_ms != planned.candidate.mtime_unix_ms ||
                !current_sidecar_identity ||
                (planned.source_sidecar_identity &&
                 (current_sidecar_identity.value().size_bytes !=
                      planned.source_sidecar_identity->size_bytes ||
                  current_sidecar_identity.value().mtime_unix_ms !=
                      planned.source_sidecar_identity->mtime_unix_ms)) ||
                !current_jpeg_identity ||
                (planned.source_jpeg_identity &&
                 (current_jpeg_identity.value().size_bytes !=
                      planned.source_jpeg_identity->size_bytes ||
                  current_jpeg_identity.value().mtime_unix_ms !=
                      planned.source_jpeg_identity->mtime_unix_ms)))
                result.source_cleanup_error =
                    make_error(ErrorCode::kConflict,
                               "Imported destination but source media or companion changed before "
                               "move cleanup",
                               {{"source", planned.candidate.source_path},
                                {"destination", planned.import_path},
                                {"reason", "import_source_changed_before_cleanup"}});
            else if (!std::filesystem::remove(utf8_path(planned.candidate.source_path),
                                              remove_error) ||
                     remove_error)
                result.source_cleanup_error = make_error(
                    ErrorCode::kIo, "Imported destination but could not remove source file",
                    {{"source", planned.candidate.source_path},
                     {"destination", planned.import_path},
                     {"reason", "import_source_cleanup_failed"},
                     {"detail", remove_error.message()}});
            if (!result.source_cleanup_error && planned.source_sidecar)
            {
                if (!std::filesystem::remove(utf8_path(*planned.source_sidecar), remove_error) ||
                    remove_error)
                    result.source_cleanup_error = make_error(
                        ErrorCode::kIo, "Imported destination but could not remove source sidecar",
                        {{"source", *planned.source_sidecar},
                         {"destination", *planned.destination_sidecar},
                         {"reason", "import_source_cleanup_failed"},
                         {"detail", remove_error.message()}});
            }
            if (!result.source_cleanup_error && planned.source_jpeg)
            {
                if (!std::filesystem::remove(utf8_path(*planned.source_jpeg), remove_error) ||
                    remove_error)
                    result.source_cleanup_error = make_error(
                        ErrorCode::kIo,
                        "Imported destination but could not remove source JPEG companion",
                        {{"source", *planned.source_jpeg},
                         {"destination", *planned.destination_jpeg},
                         {"reason", "import_source_cleanup_failed"},
                         {"detail", remove_error.message()}});
            }
        }
        switch (result.status)
        {
        case ImportItemStatus::kImported:
            ++batch.imported;
            break;
        case ImportItemStatus::kDuplicate:
            ++batch.duplicates;
            break;
        case ImportItemStatus::kUnsupported:
            ++batch.unsupported;
            break;
        case ImportItemStatus::kFailed:
            ++batch.failed;
            break;
        }
        if (result.source_cleanup_error)
            ++batch.source_cleanup_failed;
        if (result.copies_verified && result.status == ImportItemStatus::kImported)
            ++batch.verified_second_copies;
        batch.items.push_back(std::move(result));
        if (progress)
            progress(index + 1U, plan.size(), &batch.items.back());
        if (batch.items.back().source_cleanup_error || stop_after_result)
            break;
    }
    return batch;
}

} // namespace ravo
