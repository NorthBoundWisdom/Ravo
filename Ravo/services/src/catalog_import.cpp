#include "ravo/services/catalog_service.h"

#include <algorithm>
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
    std::optional<std::string> destination_sidecar;
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

void remove_owned_file(const std::optional<std::string> &path)
{
    if (!path)
        return;
    std::error_code ignored;
    static_cast<void>(std::filesystem::remove(utf8_path(*path), ignored));
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
        if (!relative_error && !relative.empty() && !relative.is_absolute())
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
    if (!is_raw_extension(utf8_path(location.value().path)))
    {
        auto decoded = raster_->decode(location.value().path, kThumbnailMaxEdge, cancellation);
        if (!decoded)
            return decoded.error();
        auto value = std::move(decoded).value();
        RasterBuffer result;
        result.width = value.width;
        result.height = value.height;
        result.source_width = value.source_width;
        result.source_height = value.source_height;
        result.srgb = std::move(value.rgb);
        result.color_profile = std::move(value.color_profile);
        return result;
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
    auto value = std::move(decoded).value();
    RasterBuffer result;
    result.width = value.width;
    result.height = value.height;
    result.source_width = value.source_width;
    result.source_height = value.source_height;
    result.srgb = std::move(value.rgb);
    result.color_profile = std::move(value.color_profile);
    return result;
}

Result<ImportBatchResult> CatalogService::execute_import(
    const ImportRequest &request,
    const std::function<void(std::size_t, std::size_t, const ImportItemResult *)> &progress)
{
    if (request.inputs.empty())
        return make_error(ErrorCode::kInvalidArgument, "Import requires at least one input");
    if (request.mode == ImportTransferMode::kAdd && !request.destination_directory.empty())
        return make_error(ErrorCode::kInvalidArgument,
                          "Add import must not specify a destination directory");
    if (request.mode != ImportTransferMode::kAdd && request.destination_directory.empty())
        return make_error(ErrorCode::kInvalidArgument,
                          "Copy and move import require a destination directory");

    auto paths = collect_import_paths(request.inputs, request.cancellation, request.recursive);
    if (!paths)
        return paths.error();
    std::filesystem::path destination_root;
    if (request.mode != ImportTransferMode::kAdd)
    {
        auto destination = normalize_local_input(request.destination_directory);
        if (!destination)
            return destination.error();
        destination_root = utf8_path(destination.value().path);
        std::error_code error;
        if (!std::filesystem::is_directory(destination_root, error) || error)
            return make_error(
                ErrorCode::kInvalidArgument, "Import destination is not an existing directory",
                {{"path", destination.value().path}, {"reason", "invalid_import_destination"}});
    }

    std::string source_root = request.source_root.value_or(request.inputs.front());
    std::error_code root_error;
    if (std::filesystem::is_regular_file(utf8_path(source_root), root_error) && !root_error)
        source_root = path_text(utf8_path(source_root).parent_path());
    std::vector<PlannedImport> plan;
    plan.reserve(paths.value().size());
    std::set<std::string, std::less<>> outputs;
    for (const auto &source : paths.value())
    {
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
        }
        if (request.mode != ImportTransferMode::kAdd)
        {
            std::filesystem::path relative;
            if (request.organization == ImportOrganization::kSingleFolder)
                relative = utf8_path(item.candidate.display_name);
            else if (request.organization == ImportOrganization::kCaptureDate)
                relative = utf8_path(date_directory(item.candidate)) /
                           utf8_path(item.candidate.display_name);
            else
            {
                const auto root_name = utf8_path(source_root).filename();
                if (root_name.empty())
                    return make_error(ErrorCode::kValidation,
                                      "Preserved hierarchy requires a named source root",
                                      {{"reason", "import_source_root_unnamed"}});
                relative = root_name / utf8_path(item.candidate.relative_path);
            }
            const auto output = destination_root / relative;
            item.import_path = path_text(output);
            if (!outputs.insert(item.import_path).second)
                return make_error(
                    ErrorCode::kConflict, "Import destination contains duplicate planned paths",
                    {{"output", item.import_path}, {"reason", "duplicate_import_output"}});
            std::error_code target_error;
            if (std::filesystem::exists(std::filesystem::symlink_status(output, target_error)) &&
                !target_error)
                return make_error(
                    ErrorCode::kConflict, "Import destination already exists",
                    {{"output", item.import_path}, {"reason", "import_destination_conflict"}});
            if (target_error && target_error != std::errc::no_such_file_or_directory)
                return make_error(ErrorCode::kIo, "Unable to inspect import destination",
                                  {{"output", item.import_path},
                                   {"detail", target_error.message()},
                                   {"reason", "import_destination_inspect_failed"}});
            auto normalized_output = normalize_local_input(item.import_path);
            if (!normalized_output)
                return normalized_output.error();
            auto existing = repository_->find_asset_by_uri(normalized_output.value().uri);
            if (!existing)
                return existing.error();
            if (existing.value())
                return make_error(ErrorCode::kConflict,
                                  "Import destination is already present in the catalog",
                                  {{"output", item.import_path},
                                   {"reason", "import_destination_catalog_conflict"}});
            if (item.source_sidecar)
            {
                auto sidecar_output = output;
                sidecar_output.replace_extension(utf8_path(*item.source_sidecar).extension());
                item.destination_sidecar = path_text(sidecar_output);
                if (!outputs.insert(*item.destination_sidecar).second)
                    return make_error(ErrorCode::kConflict,
                                      "Import sidecar destination is duplicated",
                                      {{"output", *item.destination_sidecar},
                                       {"reason", "duplicate_import_output"}});
                std::error_code sidecar_error;
                if (std::filesystem::exists(
                        std::filesystem::symlink_status(sidecar_output, sidecar_error)) &&
                    !sidecar_error)
                    return make_error(ErrorCode::kConflict,
                                      "Import sidecar destination already exists",
                                      {{"output", *item.destination_sidecar},
                                       {"reason", "import_destination_conflict"}});
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
        if (request.mode != ImportTransferMode::kAdd)
        {
            std::error_code directory_error;
            std::filesystem::create_directories(utf8_path(planned.import_path).parent_path(),
                                                directory_error);
            if (directory_error)
                return make_error(ErrorCode::kIo, "Unable to create import destination directory",
                                  {{"output", planned.import_path},
                                   {"detail", directory_error.message()},
                                   {"reason", "import_destination_create_failed"}});
            auto copied = copy_file_atomically(planned.candidate.source_path, planned.import_path,
                                               request.cancellation);
            if (!copied)
                return copied.error();
            if (planned.source_sidecar)
            {
                auto copied_sidecar = copy_file_atomically(
                    *planned.source_sidecar, *planned.destination_sidecar, request.cancellation);
                if (!copied_sidecar)
                {
                    remove_owned_file(planned.import_path);
                    return copied_sidecar.error();
                }
            }
        }
        auto imported = import_one(planned.import_path, request.cancellation, request.preview,
                                   request.defer_previews);
        ImportItemResult result = imported ?
                                      std::move(imported).value() :
                                      failed_item(planned.candidate.source_path, imported.error());
        result.input_path = planned.candidate.source_path;
        if (request.mode != ImportTransferMode::kAdd)
        {
            result.destination_path = planned.import_path;
            result.sidecar_destination_path = planned.destination_sidecar;
            if (result.status == ImportItemStatus::kFailed ||
                result.status == ImportItemStatus::kUnsupported)
            {
                remove_owned_file(planned.destination_sidecar);
                remove_owned_file(planned.import_path);
            }
            else if (result.status == ImportItemStatus::kDuplicate)
            {
                result.error =
                    make_error(ErrorCode::kConflict,
                               "Import destination was cataloged concurrently after publication",
                               {{"output", planned.import_path},
                                {"reason", "import_destination_catalog_race"}});
            }
        }
        if (request.mode == ImportTransferMode::kMove &&
            result.status == ImportItemStatus::kImported)
        {
            std::error_code remove_error;
            auto current_identity = read_file_identity(planned.candidate.source_path);
            if (!current_identity ||
                current_identity.value().size_bytes != planned.candidate.size_bytes ||
                current_identity.value().mtime_unix_ms != planned.candidate.mtime_unix_ms)
                result.source_cleanup_error =
                    make_error(ErrorCode::kConflict,
                               "Imported destination but source changed before move cleanup",
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
        batch.items.push_back(std::move(result));
        if (progress)
            progress(index + 1U, plan.size(), &batch.items.back());
        if (batch.items.back().source_cleanup_error)
            break;
    }
    return batch;
}

} // namespace ravo
