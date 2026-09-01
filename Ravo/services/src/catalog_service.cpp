#include "ravo/services/catalog_service.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <utility>

#include "catalog_internal.h"
#include "catalog_service_test_support.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{

void testing::CatalogServiceTestControl::set_before_import_publication(
    CatalogService &service, std::function<void()> callback)
{
    service.testing_before_import_publication_ = std::move(callback);
}

void testing::CatalogServiceTestControl::set_before_preview_cache_publication(
    CatalogService &service, std::function<void()> callback)
{
    service.testing_before_preview_cache_publication_ = std::move(callback);
}

void testing::CatalogServiceTestControl::set_backup_checkpoint(
    CatalogService &service,
    std::function<Result<void>(std::string_view checkpoint, std::string_view path)> callback)
{
    service.testing_backup_checkpoint_ = std::move(callback);
}

std::array<std::optional<std::uint32_t>, 2>
testing::CatalogServiceTestControl::linear_working_max_edges(const CatalogService &service)
{
    std::array<std::optional<std::uint32_t>, 2> result;
    for (std::size_t index = 0; index < service.linear_working_.size(); ++index)
    {
        if (service.linear_working_[index])
        {
            result[index] = service.linear_working_[index]->max_edge;
        }
    }
    return result;
}

std::optional<std::uint32_t>
testing::CatalogServiceTestControl::browse_linear_working_max_edge(const CatalogService &service)
{
    if (!service.browse_linear_working_)
    {
        return std::nullopt;
    }
    return service.browse_linear_working_->max_edge;
}
namespace
{

[[nodiscard]] bool media_type_has_embedded_capture(const std::string_view media_type) noexcept
{
    return is_raw_media_type(media_type) || media_type == kMediaTypeJpeg ||
           media_type == kMediaTypePng || media_type == kMediaTypeTiff;
}

[[nodiscard]] bool is_common_raster_media(const std::string_view media_type) noexcept
{
    return media_type == kMediaTypeJpeg || media_type == kMediaTypePng ||
           media_type == kMediaTypeTiff;
}

void merge_engine_capture(CaptureMetadata &target, const EngineCaptureMetadata &source)
{
    if (source.camera_make)
        target.camera_make = source.camera_make;
    if (source.camera_model)
        target.camera_model = source.camera_model;
    if (source.iso)
        target.iso = source.iso;
    if (source.aperture)
        target.aperture = source.aperture;
    if (source.focal_length_mm)
        target.focal_length_mm = source.focal_length_mm;
    if (source.shutter_s)
        target.shutter_s = source.shutter_s;
    if (source.captured_datetime)
    {
        CaptureDateTime captured;
        captured.local_exif = source.captured_datetime->local_exif;
        captured.subsecond_digits = source.captured_datetime->subsecond_digits;
        captured.utc_offset_minutes = source.captured_datetime->utc_offset_minutes;
        target.captured_datetime = std::move(captured);
    }
    if (source.location)
    {
        CaptureLocation copied;
        copied.latitude_e6 = source.location->latitude_e6;
        copied.longitude_e6 = source.location->longitude_e6;
        if (source.location->altitude)
        {
            CaptureAltitude altitude;
            altitude.magnitude_mm = source.location->altitude->magnitude_mm;
            altitude.reference = source.location->altitude->reference ==
                                         EngineCaptureAltitudeReference::kBelowSeaLevel ?
                                     CaptureAltitudeReference::kBelowSeaLevel :
                                     CaptureAltitudeReference::kAboveSeaLevel;
            copied.altitude = altitude;
        }
        target.location = copied;
    }
}

[[nodiscard]] std::string_view context_value(const TaskError &error, const std::string_view key)
{
    const auto found = error.context.find(std::string(key));
    if (found == error.context.end())
    {
        return {};
    }
    return found->second;
}

[[nodiscard]] bool is_recognized_raster_probe_error(const TaskError &error) noexcept
{
    const auto format = context_value(error, "format");
    return format == "jpeg" || format == "jpg" || format == "png" || format == "tiff" ||
           format == "tif" || format == "bmp" || format == "gif" || format == "webp" ||
           format == "qoi" || format == "rgbe";
}

[[nodiscard]] bool should_try_raw_after_raster(const TaskError &error) noexcept
{
    const auto format = context_value(error, "format");
    const auto reason = context_value(error, "reason");
    if ((format == "tiff" || format == "tif") && reason == "unsupported_tiff_raw_container")
    {
        return true;
    }
    if (is_recognized_raster_probe_error(error))
    {
        return false;
    }
    return error.code == ErrorCode::kUnsupported;
}

[[nodiscard]] std::string utf8_string(const std::u8string &value)
{
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}

[[nodiscard]] TaskError
annotate_batch_export_error(TaskError error, const std::size_t completed_count,
                            const std::size_t total_count, const std::size_t failed_index,
                            const std::string_view asset_id, const std::string_view output)
{
    error.context.insert_or_assign("asset_id", std::string(asset_id));
    error.context.insert_or_assign("batch_index", std::to_string(failed_index + 1U));
    error.context.insert_or_assign("completed_count", std::to_string(completed_count));
    error.context.insert_or_assign("output", std::string(output));
    error.context.insert_or_assign("partial_batch", completed_count == 0 ? "false" : "true");
    error.context.insert_or_assign("total_count", std::to_string(total_count));
    return error;
}

} // namespace

CatalogService::CatalogService(const EngineFacade &engine,
                               std::unique_ptr<CatalogRepository> repository,
                               std::unique_ptr<RasterDecoder> raster,
                               std::unique_ptr<PreviewCache> cache,
                               std::unique_ptr<RecoveryStore> recovery)
    : engine_(&engine)
    , repository_(std::move(repository))
    , raster_(std::move(raster))
    , cache_(std::move(cache))
    , recovery_(std::move(recovery))
{
}

CatalogService::~CatalogService()
{
    static_cast<void>(close());
}

Result<void> CatalogService::close()
{
    if (repository_ == nullptr)
    {
        return {};
    }
    std::optional<TaskError> recovery_error;
    if (recovery_ != nullptr)
    {
        auto synchronized = sync_recovery(std::nullopt);
        if (!synchronized)
        {
            recovery_error = synchronized.error();
        }
    }
    const auto closed = repository_->close();
    repository_.reset();
    raster_.reset();
    cache_.reset();
    recovery_.reset();
    engine_ = nullptr;
    decoded_preview_source_.reset();
    decoded_raw_.reset();
    for (auto &working : linear_working_)
    {
        working.reset();
    }
    browse_decoded_preview_source_.reset();
    browse_decoded_raw_.reset();
    browse_linear_working_.reset();
    if (recovery_error)
    {
        if (!closed)
        {
            recovery_error->context.insert_or_assign("catalog_close_failed", "true");
            recovery_error->context.insert_or_assign("catalog_close_error", closed.error().message);
        }
        return *recovery_error;
    }
    return closed;
}

Result<CatalogSnapshot> CatalogService::snapshot() const
{
    if (repository_ == nullptr || cache_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto snapshot = repository_->snapshot();
    if (!snapshot)
    {
        return snapshot.error();
    }
    snapshot.value().cache_root = cache_->root();
    return snapshot;
}

Result<std::vector<AssetRecord>> CatalogService::list_assets() const
{
    return list_assets(LibraryQuery{});
}

Result<std::vector<AssetRecord>> CatalogService::list_assets(const LibraryQuery &query) const
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto valid_query = validate_library_query(query);
    if (!valid_query)
    {
        return valid_query.error();
    }
    auto listed = repository_->list_assets();
    if (!listed)
    {
        return listed.error();
    }
    return filter_and_sort_assets(std::move(listed).value(), query);
}

Result<LibraryPage> CatalogService::list_assets_page(const LibraryPageRequest &request) const
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto valid = validate_library_page_request(request);
    if (!valid)
        return valid.error();
    return repository_->list_assets_page(request);
}

Result<std::vector<PreviewRecord>> CatalogService::list_previews() const
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    return repository_->list_previews();
}

Result<std::vector<PreviewRecord>>
CatalogService::list_previews_for_assets(const std::vector<std::string> &asset_ids) const
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return repository_->list_previews_for_assets(asset_ids);
}

Result<AssetRecoveryState> CatalogService::recovery_state(const std::string_view asset_id) const
{
    if (repository_ == nullptr || recovery_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    return repository_->recovery_state(asset_id);
}

Result<std::vector<AssetRecoveryState>> CatalogService::pending_recovery() const
{
    if (repository_ == nullptr || recovery_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    return repository_->list_pending_recovery();
}

Result<RecoveryArtifact>
CatalogService::synchronize_recovery_asset(const std::string_view asset_id,
                                           const CancellationToken &cancellation)
{
    if (repository_ == nullptr || recovery_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    auto state = repository_->recovery_state(asset_id);
    if (!state)
    {
        return state.error();
    }
    if (!state.value().pending())
    {
        return recovery_->verify(asset_id, state.value().generation, cancellation);
    }
    auto snapshot = repository_->load_recovery_snapshot(asset_id);
    if (!snapshot)
    {
        return snapshot.error();
    }
    auto artifact = recovery_->publish(snapshot.value(), cancellation);
    if (!artifact)
    {
        return artifact.error();
    }
    auto acknowledged =
        repository_->acknowledge_recovery(asset_id, snapshot.value().state.generation);
    if (!acknowledged)
    {
        auto error = acknowledged.error();
        error.context.insert_or_assign("sidecar_published", "true");
        error.context.insert_or_assign("sidecar_path", artifact.value().path);
        return error;
    }
    auto cleaned = recovery_->remove_older(asset_id, snapshot.value().state.generation);
    if (!cleaned)
    {
        auto error = cleaned.error();
        error.context.insert_or_assign("recovery_acknowledged", "true");
        error.context.insert_or_assign("sidecar_published", "true");
        error.context.insert_or_assign("sidecar_path", artifact.value().path);
        return error;
    }
    return artifact;
}

Result<void> CatalogService::synchronize_committed_change(const std::string_view asset_id,
                                                          const CancellationToken &cancellation)
{
    auto synchronized = synchronize_recovery_asset(asset_id, cancellation);
    if (!synchronized)
    {
        auto error = synchronized.error();
        error.context.insert_or_assign("asset_id", std::string(asset_id));
        error.context.insert_or_assign("catalog_committed", "true");
        auto state = repository_->recovery_state(asset_id);
        if (state)
        {
            error.context.insert_or_assign("recovery_generation",
                                           std::to_string(state.value().generation));
            error.context.insert_or_assign("recovery_synchronized_generation",
                                           std::to_string(state.value().synchronized_generation));
            error.context.insert_or_assign("recovery_pending",
                                           state.value().pending() ? "true" : "false");
        }
        else
        {
            error.context.insert_or_assign("recovery_pending", "unknown");
            error.context.insert_or_assign("recovery_state_error", state.error().message);
        }
        return error;
    }
    return {};
}

Result<RecoverySyncResult>
CatalogService::sync_recovery(const std::optional<std::string_view> asset_id,
                              const CancellationToken &cancellation)
{
    if (repository_ == nullptr || recovery_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    RecoverySyncResult result;
    result.root = recovery_->root();
    if (asset_id)
    {
        auto state = repository_->recovery_state(*asset_id);
        if (!state)
        {
            return state.error();
        }
        result.pending_before = state.value().pending() ? 1U : 0U;
        auto artifact = synchronize_recovery_asset(*asset_id, cancellation);
        if (!artifact)
        {
            return artifact.error();
        }
        result.artifacts.push_back(std::move(artifact).value());
    }
    else
    {
        auto pending = repository_->list_pending_recovery();
        if (!pending)
        {
            return pending.error();
        }
        result.pending_before = pending.value().size();
        result.artifacts.reserve(pending.value().size());
        for (const auto &state : pending.value())
        {
            auto active = cancellation.check();
            if (!active)
            {
                auto error = active.error();
                error.context.insert_or_assign("completed_count",
                                               std::to_string(result.artifacts.size()));
                return error;
            }
            auto artifact = synchronize_recovery_asset(state.asset_id, cancellation);
            if (!artifact)
            {
                auto error = artifact.error();
                error.context.insert_or_assign("asset_id", state.asset_id);
                error.context.insert_or_assign("completed_count",
                                               std::to_string(result.artifacts.size()));
                return error;
            }
            result.artifacts.push_back(std::move(artifact).value());
        }
    }
    auto remaining = repository_->list_pending_recovery();
    if (!remaining)
    {
        return remaining.error();
    }
    result.pending_after = remaining.value().size();
    return result;
}

Result<std::vector<FolderRecord>> CatalogService::list_folders() const
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto folders = repository_->list_folders();
    if (!folders)
        return folders.error();
    for (auto &folder : folders.value())
    {
        if (folder.id.empty())
            continue;
        auto location = normalize_local_input(folder.uri);
        if (!location)
            return location.error();
        std::error_code error;
        folder.missing = !std::filesystem::is_directory(
            std::filesystem::path(
                std::u8string(location.value().path.begin(), location.value().path.end())),
            error);
        if (error)
            folder.missing = true;
    }
    return folders;
}

Result<AssetRecord> CatalogService::set_rating(const std::string_view asset_id, const int rating)
{
    auto valid = validate_rating(rating);
    if (!valid)
    {
        return valid.error();
    }
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    ReviewState review = asset.value()->review;
    review.rating = rating;
    const auto updated = repository_->update_review(asset_id, review);
    if (!updated)
    {
        return updated.error();
    }
    const auto revision = repository_->bump_revision();
    if (!revision)
    {
        return revision.error();
    }
    asset.value()->review = review;
    auto recovered = synchronize_committed_change(asset_id);
    if (!recovered)
    {
        return recovered.error();
    }
    return *asset.value();
}

Result<AssetRecord> CatalogService::set_color_label(const std::string_view asset_id,
                                                    const ColorLabel label)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    ReviewState review = asset.value()->review;
    review.color_label = label;
    const auto updated = repository_->update_review(asset_id, review);
    if (!updated)
    {
        return updated.error();
    }
    const auto revision = repository_->bump_revision();
    if (!revision)
    {
        return revision.error();
    }
    asset.value()->review = review;
    auto recovered = synchronize_committed_change(asset_id);
    if (!recovered)
    {
        return recovered.error();
    }
    return *asset.value();
}

Result<AssetRecord> CatalogService::set_rejected(const std::string_view asset_id,
                                                 const bool rejected)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    ReviewState review = asset.value()->review;
    review.rejected = rejected;
    const auto updated = repository_->update_review(asset_id, review);
    if (!updated)
    {
        return updated.error();
    }
    const auto revision = repository_->bump_revision();
    if (!revision)
    {
        return revision.error();
    }
    asset.value()->review = review;
    auto recovered = synchronize_committed_change(asset_id);
    if (!recovered)
    {
        return recovered.error();
    }
    return *asset.value();
}

Result<void> CatalogService::remove_from_catalog(const std::string_view asset_id)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    if (cache_ != nullptr)
    {
        const auto removed_cache = cache_->remove_for_asset(asset_id);
        if (!removed_cache)
        {
            return removed_cache.error();
        }
    }
    auto removed = repository_->remove_asset(asset_id);
    if (!removed)
    {
        return removed.error();
    }
    if (recovery_ != nullptr)
    {
        auto removed_recovery = recovery_->remove_asset(asset_id);
        if (!removed_recovery)
        {
            auto error = removed_recovery.error();
            error.context.insert_or_assign("asset_id", std::string(asset_id));
            error.context.insert_or_assign("catalog_removed", "true");
            return error;
        }
    }
    return {};
}

Result<void> CatalogService::remove_original_and_catalog(const std::string_view asset_id)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    auto location = normalize_local_input(asset.value()->normalized_uri);
    if (!location)
    {
        return location.error();
    }
    const auto path = std::filesystem::path(
        std::u8string(location.value().path.begin(), location.value().path.end()));
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(path, exists_error);
    if (exists_error)
    {
        return make_error(ErrorCode::kIo, "Unable to inspect original file",
                          {{"path", location.value().path},
                           {"asset_id", std::string(asset_id)},
                           {"detail", exists_error.message()}});
    }
    if (!exists)
    {
        return make_error(ErrorCode::kNotFound, "Original file is missing",
                          {{"path", location.value().path}, {"asset_id", std::string(asset_id)}});
    }
    std::error_code type_error;
    if (!std::filesystem::is_regular_file(path, type_error) || type_error)
    {
        return make_error(ErrorCode::kUnsupported, "Original path is not a regular file",
                          {{"path", location.value().path},
                           {"asset_id", std::string(asset_id)},
                           {"detail", type_error.message()},
                           {"reason", "original_delete_non_regular"}});
    }
    std::filesystem::path quarantine;
    for (std::uint32_t suffix = 0U; suffix < 1024U; ++suffix)
    {
        std::filesystem::path candidate = path;
        candidate += ".ravo-delete-" + std::to_string(suffix);
        std::error_code candidate_error;
        const bool occupied = std::filesystem::exists(candidate, candidate_error);
        if (candidate_error)
        {
            return make_error(ErrorCode::kIo, "Unable to inspect delete quarantine path",
                              {{"path", location.value().path},
                               {"asset_id", std::string(asset_id)},
                               {"detail", candidate_error.message()},
                               {"reason", "delete_quarantine_inspect_failed"}});
        }
        if (!occupied)
        {
            quarantine = candidate;
            break;
        }
    }
    if (quarantine.empty())
    {
        return make_error(ErrorCode::kConflict, "No unique delete quarantine path is available",
                          {{"path", location.value().path},
                           {"asset_id", std::string(asset_id)},
                           {"reason", "delete_quarantine_conflict"}});
    }
    std::error_code rename_error;
    std::filesystem::rename(path, quarantine, rename_error);
    if (rename_error)
    {
        return make_error(ErrorCode::kIo, "Unable to quarantine original before deletion",
                          {{"path", location.value().path},
                           {"asset_id", std::string(asset_id)},
                           {"detail", rename_error.message()},
                           {"reason", "delete_quarantine_rename_failed"}});
    }
    auto removed = remove_from_catalog(asset_id);
    std::optional<TaskError> cleanup_error;
    if (!removed)
    {
        TaskError primary = removed.error();
        const auto committed = primary.context.find("catalog_removed");
        if (committed != primary.context.end() && committed->second == "true")
        {
            cleanup_error = std::move(primary);
        }
        else
        {
            std::error_code rollback_error;
            std::filesystem::rename(quarantine, path, rollback_error);
            if (rollback_error)
            {
                primary.context.insert_or_assign("rollback_failed", "true");
                primary.context.insert_or_assign("rollback_error", rollback_error.message());
                primary.context.insert_or_assign("quarantine_path", quarantine.string());
            }
            return primary;
        }
    }
    std::error_code remove_error;
    if (!std::filesystem::remove(quarantine, remove_error) || remove_error)
    {
        auto error =
            make_error(ErrorCode::kIo,
                       "Catalog entry was removed but quarantined original could not be deleted",
                       {{"path", location.value().path},
                        {"quarantine_path", quarantine.string()},
                        {"asset_id", std::string(asset_id)},
                        {"catalog_removed", "true"},
                        {"detail", remove_error.message()},
                        {"reason", "delete_quarantine_finalize_failed"}});
        if (cleanup_error)
        {
            error.context.insert_or_assign("recovery_cleanup_failed", "true");
            error.context.insert_or_assign("recovery_cleanup_error", cleanup_error->message);
        }
        return error;
    }
    if (cleanup_error)
    {
        cleanup_error->context.insert_or_assign("original_removed", "true");
        return *cleanup_error;
    }
    return {};
}

Result<AssetRecord> CatalogService::refresh_capture_metadata(const std::string_view asset_id,
                                                             const CancellationToken &cancellation)
{
    auto active = cancellation.check();
    if (!active)
        return active.error();
    if (repository_ == nullptr || engine_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto existing = repository_->find_asset_by_id(asset_id);
    if (!existing)
        return existing.error();
    if (!existing.value())
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    auto location = normalize_local_input(existing.value()->normalized_uri);
    if (!location)
        return location.error();
    auto identity = read_file_identity(location.value().path);
    if (!identity)
        return identity.error();

    CaptureMetadata refreshed;
    if (is_raw_media_type(existing.value()->media_type))
    {
        auto inspected = engine_->inspect(location.value().path, cancellation);
        if (!inspected)
            return inspected.error();
        if (!inspected.value().is_raw)
            return make_error(ErrorCode::kValidation,
                              "Catalog RAW asset no longer identifies as RAW",
                              {{"asset_id", std::string(asset_id)},
                               {"reason", "metadata_refresh_media_mismatch"}});
        if (!inspected.value().make.empty())
            refreshed.camera_make = inspected.value().make;
        if (!inspected.value().model.empty())
            refreshed.camera_model = inspected.value().model;
        refreshed.iso = inspected.value().iso;
        refreshed.aperture = inspected.value().aperture;
        refreshed.focal_length_mm = inspected.value().focal_length_mm;
        refreshed.shutter_s = inspected.value().shutter_s;
        refreshed.captured_unix_s = inspected.value().captured_unix_s;
    }
    if (media_type_has_embedded_capture(existing.value()->media_type))
    {
        auto extracted =
            engine_->read_embedded_capture_metadata(location.value().path, cancellation);
        if (!extracted)
            return extracted.error();
        merge_engine_capture(refreshed, extracted.value());
    }
    auto valid = validate_capture_metadata(refreshed);
    if (!valid)
        return valid.error();
    active = cancellation.check();
    if (!active)
        return active.error();
    AssetRecord updated = *existing.value();
    updated.size_bytes = identity.value().size_bytes;
    updated.mtime_unix_ms = identity.value().mtime_unix_ms;
    updated.content_fingerprint = make_content_fingerprint(identity.value());
    updated.import_state = std::string(kImportStateImported);
    updated.error_code.reset();
    updated.error_message.reset();
    updated.capture = std::move(refreshed);
    auto published = repository_->commit_refreshed_asset(updated);
    if (!published)
        return published.error();
    auto recovered = synchronize_committed_change(asset_id, cancellation);
    if (!recovered)
        return recovered.error();
    return updated;
}

Result<bool> CatalogService::asset_has_edits(const std::string_view asset_id) const
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    return asset.value()->has_edits;
}

Result<Recipe> CatalogService::load_baseline_recipe(const std::string_view asset_id) const
{
    if (repository_ == nullptr || engine_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    auto location = normalize_local_input(asset.value()->normalized_uri);
    if (!location)
    {
        return location.error();
    }
    return baseline_recipe_for(*asset.value(), location.value().path);
}

Result<Recipe> CatalogService::load_recipe(const std::string_view asset_id) const
{
    if (repository_ == nullptr || engine_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto baseline = load_baseline_recipe(asset_id);
    if (!baseline)
    {
        return baseline.error();
    }
    auto stored = repository_->load_recipe_json(asset_id);
    if (!stored)
    {
        return stored.error();
    }
    if (!stored.value())
    {
        return baseline;
    }
    auto parsed = parse_recipe_json(*stored.value());
    if (!parsed)
    {
        return parsed.error();
    }
    parsed.value().asset = baseline.value().asset;
    auto valid = engine_->validate(parsed.value());
    if (!valid)
    {
        return valid.error();
    }
    return parsed;
}

Result<AssetRecord> CatalogService::save_recipe(const std::string_view asset_id,
                                                const Recipe &recipe,
                                                const RecipeSaveOptions options)
{
    auto saved = save_recipe_with_history(asset_id, recipe, options);
    if (!saved)
    {
        return saved.error();
    }
    return std::move(saved).value().asset;
}

Result<RecipeSaveResult> CatalogService::save_recipe_with_history(const std::string_view asset_id,
                                                                  const Recipe &recipe,
                                                                  const RecipeSaveOptions options)
{
    if (repository_ == nullptr || engine_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    auto location = normalize_local_input(asset.value()->normalized_uri);
    if (!location)
    {
        return location.error();
    }
    Recipe stored = recipe;
    stored.asset = {asset.value()->id, location.value().path, asset.value()->content_fingerprint};
    auto valid = engine_->validate(stored);
    if (!valid)
    {
        return valid.error();
    }
    auto lut_fingerprint = engine_->lut3d_cache_fingerprint(stored);
    if (!lut_fingerprint)
    {
        return lut_fingerprint.error();
    }
    auto params = develop_from_recipe(stored);
    if (!params)
    {
        return params.error();
    }
    std::optional<std::string> recipe_json;
    if (matches_develop_baseline(*asset.value(), params.value()))
    {
        recipe_json.reset();
    }
    else
    {
        auto json = serialize_recipe(stored);
        if (!json)
        {
            return json.error();
        }
        recipe_json = std::move(json).value();
    }
    const std::optional<std::string_view> recipe_json_view =
        recipe_json ? std::optional<std::string_view>{*recipe_json} : std::nullopt;
    // Keep an owned, non-null empty string for the explicit baseline history entry. A default
    // string_view has a null data pointer, which Qt Sql correctly binds as SQL NULL.
    const std::string history_json = recipe_json.value_or(std::string{});
    const auto committed = repository_->commit_recipe(
        asset_id, stored.schema_version, recipe_json_view, history_json, options.history_write,
        options.discard_history_after_seq, options.coalesce_history_id);
    if (!committed)
    {
        return committed.error();
    }
    asset.value()->has_edits = recipe_json.has_value();
    if (!options.defer_recovery_publication)
    {
        auto recovered = synchronize_committed_change(asset_id);
        if (!recovered)
        {
            return recovered.error();
        }
    }
    return RecipeSaveResult{*asset.value(), committed.value().revision,
                            committed.value().history_id};
}

Result<AssetRecord> CatalogService::save_develop(const std::string_view asset_id,
                                                 const DevelopParams &params,
                                                 const RecipeSaveOptions options)
{
    auto saved = save_develop_with_history(asset_id, params, options);
    if (!saved)
    {
        return saved.error();
    }
    return std::move(saved).value().asset;
}

Result<RecipeSaveResult> CatalogService::save_develop_with_history(const std::string_view asset_id,
                                                                   const DevelopParams &params,
                                                                   const RecipeSaveOptions options)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    auto location = normalize_local_input(asset.value()->normalized_uri);
    if (!location)
    {
        return location.error();
    }
    DevelopParams stored = params;
    if (stored.lens_mode == kLensModeLookup)
    {
        if (stored.lens_make.empty() && asset.value()->capture.camera_make)
        {
            stored.lens_make = *asset.value()->capture.camera_make;
        }
        if (stored.lens_model.empty() && asset.value()->capture.camera_model)
        {
            stored.lens_model = *asset.value()->capture.camera_model;
        }
        if (stored.lens_focal_mm <= 0.0 && asset.value()->capture.focal_length_mm)
        {
            stored.lens_focal_mm = *asset.value()->capture.focal_length_mm;
        }
    }
    auto recipe = recipe_from_develop(
        {asset.value()->id, location.value().path, asset.value()->content_fingerprint}, stored);
    if (!recipe)
    {
        return recipe.error();
    }
    return save_recipe_with_history(asset_id, recipe.value(), options);
}

Result<std::array<double, 4>>
CatalogService::sample_white_balance(const std::string_view asset_id,
                                     const WhiteBalancePickRequest &request,
                                     const CancellationToken &cancellation)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    if (!is_raw_media_type(asset.value()->media_type))
    {
        return make_error(ErrorCode::kUnsupported,
                          "White-balance pick requires a Bayer RAW original",
                          {{"media_type", asset.value()->media_type}});
    }
    auto location = normalize_local_input(asset.value()->normalized_uri);
    if (!location)
    {
        return location.error();
    }
    auto decoded = engine_->decode_raw_frame(location.value().path, cancellation);
    if (!decoded)
    {
        return decoded.error();
    }
    return engine_->sample_white_balance(decoded.value(), request);
}

Result<AssetRecord> CatalogService::reset_recipe(const std::string_view asset_id)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    return save_develop(asset_id, baseline_develop_for(*asset.value()));
}

Result<AssetRecord> CatalogService::set_tags(const std::string_view asset_id,
                                             const std::vector<std::string> &tags)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    std::vector<std::string> normalized;
    normalized.reserve(tags.size());
    for (const auto &tag : tags)
    {
        auto parsed = normalize_tag_name(tag);
        if (!parsed)
        {
            return parsed.error();
        }
        if (std::find(normalized.begin(), normalized.end(), parsed.value()) == normalized.end())
        {
            normalized.push_back(std::move(parsed).value());
        }
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    const auto saved = repository_->replace_asset_tags(asset_id, normalized);
    if (!saved)
    {
        return saved.error();
    }
    const auto revision = repository_->bump_revision();
    if (!revision)
    {
        return revision.error();
    }
    asset.value()->tags = std::move(normalized);
    auto recovered = synchronize_committed_change(asset_id);
    if (!recovered)
    {
        return recovered.error();
    }
    return *asset.value();
}

Result<AssetRecord> CatalogService::set_writable_metadata(const std::string_view asset_id,
                                                          const WritableMetadata &metadata)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    const auto check_field = [](const std::string_view name,
                                const std::optional<std::string> &value) -> Result<void>
    {
        if (!value)
        {
            return {};
        }
        return validate_metadata_field(name, *value);
    };
    auto title = check_field("title", metadata.title);
    if (!title)
    {
        return title.error();
    }
    auto description = check_field("description", metadata.description);
    if (!description)
    {
        return description.error();
    }
    auto creator = check_field("creator", metadata.creator);
    if (!creator)
    {
        return creator.error();
    }
    auto copyright = check_field("copyright", metadata.copyright);
    if (!copyright)
    {
        return copyright.error();
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    const auto saved = repository_->upsert_writable_metadata(asset_id, metadata);
    if (!saved)
    {
        return saved.error();
    }
    const auto revision = repository_->bump_revision();
    if (!revision)
    {
        return revision.error();
    }
    asset.value()->metadata = metadata;
    auto recovered = synchronize_committed_change(asset_id);
    if (!recovered)
    {
        return recovered.error();
    }
    return *asset.value();
}

Result<std::vector<RecipeHistoryEntry>>
CatalogService::list_recipe_history(const std::string_view asset_id) const
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    return repository_->list_recipe_history(asset_id);
}

Result<AssetRecord> CatalogService::create_recipe_snapshot(const std::string_view asset_id,
                                                           const std::string_view label)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto trimmed = normalize_tag_name(label);
    if (!trimmed)
    {
        return trimmed.error();
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    auto json = repository_->load_recipe_json(asset_id);
    if (!json)
    {
        return json.error();
    }
    const std::string recipe_json = json.value().value_or(std::string{});
    auto recorded = repository_->append_recipe_history(
        asset_id, kRecipeHistoryKindSnapshot, std::string_view{trimmed.value()}, recipe_json);
    if (!recorded)
    {
        return recorded.error();
    }
    const auto revision = repository_->bump_revision();
    if (!revision)
    {
        return revision.error();
    }
    auto recovered = synchronize_committed_change(asset_id);
    if (!recovered)
    {
        return recovered.error();
    }
    return *asset.value();
}

Result<AssetRecord> CatalogService::rename_recipe_snapshot(const std::string_view asset_id,
                                                           const std::int64_t history_id,
                                                           const std::string_view label)
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto trimmed = normalize_tag_name(label);
    if (!trimmed)
    {
        return trimmed.error();
    }
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", std::string(asset_id)}});
    }
    auto entry = repository_->find_recipe_history(history_id);
    if (!entry)
    {
        return entry.error();
    }
    if (!entry.value() || entry.value()->asset_id != asset_id)
    {
        return make_error(ErrorCode::kNotFound, "Recipe snapshot does not exist",
                          {{"history_id", std::to_string(history_id)}});
    }
    if (entry.value()->kind != kRecipeHistoryKindSnapshot)
    {
        return make_error(ErrorCode::kValidation, "Only snapshots can be renamed",
                          {{"kind", entry.value()->kind}});
    }
    auto updated = repository_->update_recipe_history_label(history_id, trimmed.value());
    if (!updated)
    {
        return updated.error();
    }
    const auto revision = repository_->bump_revision();
    if (!revision)
    {
        return revision.error();
    }
    auto recovered = synchronize_committed_change(asset_id);
    if (!recovered)
    {
        return recovered.error();
    }
    return *asset.value();
}

Result<AssetRecord> CatalogService::restore_recipe_history(const std::string_view asset_id,
                                                           const std::int64_t history_id)
{
    if (repository_ == nullptr || engine_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto entry = repository_->find_recipe_history(history_id);
    if (!entry)
    {
        return entry.error();
    }
    if (!entry.value() || entry.value()->asset_id != asset_id)
    {
        return make_error(ErrorCode::kNotFound, "Recipe history entry does not exist",
                          {{"history_id", std::to_string(history_id)}});
    }
    if (entry.value()->recipe_json.empty())
    {
        return reset_recipe(asset_id);
    }
    auto parsed = parse_recipe_json(entry.value()->recipe_json);
    if (!parsed)
    {
        return parsed.error();
    }
    return save_recipe(asset_id, parsed.value());
}

Result<ImportItemResult> CatalogService::import_one(const std::string_view path,
                                                    const CancellationToken &cancellation,
                                                    const ImportPreviewPolicy preview_policy,
                                                    const bool defer_preview)
{
    auto cancelled = cancellation.check();
    if (!cancelled)
    {
        return failed_item(std::string(path), cancelled.error());
    }
    if (repository_ == nullptr || raster_ == nullptr || engine_ == nullptr || cache_ == nullptr)
    {
        return failed_item(std::string(path),
                           make_error(ErrorCode::kIo, "Catalog session is closed"));
    }

    LOG_INFO(ravo::logger(), "import one path={}", path);
    auto location = normalize_local_input(path);
    if (!location)
    {
        LOG_ERROR(ravo::logger(), "import path normalize failed path={} error={}", path,
                  location.error().message);
        return failed_item(std::string(path), location.error());
    }
    LOG_DEBUG(ravo::logger(), "import normalized path={} uri={}", location.value().path,
              location.value().uri);

    std::error_code exists_error;
    if (!std::filesystem::is_regular_file(
            std::filesystem::path(
                std::u8string(location.value().path.begin(), location.value().path.end())),
            exists_error) ||
        exists_error)
    {
        return failed_item(location.value().path,
                           make_error(ErrorCode::kNotFound, "Import input does not exist",
                                      {{"path", location.value().path}}));
    }

    auto existing = repository_->find_asset_by_uri(location.value().uri);
    if (!existing)
    {
        return failed_item(location.value().path, existing.error());
    }
    if (existing.value())
    {
        ImportItemResult duplicate;
        duplicate.status = ImportItemStatus::kDuplicate;
        duplicate.input_path = location.value().path;
        duplicate.asset = *existing.value();
        return duplicate;
    }

    auto identity = read_file_identity(location.value().path);
    if (!identity)
    {
        return failed_item(location.value().path, identity.error());
    }

    AssetRecord asset;
    asset.id = generate_asset_id();
    asset.normalized_uri = location.value().uri;
    asset.size_bytes = identity.value().size_bytes;
    asset.mtime_unix_ms = identity.value().mtime_unix_ms;
    asset.content_fingerprint = make_content_fingerprint(identity.value());
    asset.created_unix_ms = now_unix_ms();
    asset.import_state = std::string(kImportStateImported);

    const std::filesystem::path file_path(
        std::u8string(location.value().path.begin(), location.value().path.end()));
    std::optional<EmbeddedPreview> embedded_preview;
    std::optional<DecodedRaster> validated_raster;
    const auto apply_inspection = [&](const InspectionResult &inspected) -> Result<void>
    {
        if (!inspected.is_raw)
        {
            return make_error(ErrorCode::kUnsupported, "Input is not a supported RAW file",
                              {{"path", location.value().path}});
        }
        asset.media_type = std::string(kMediaTypeRaw);
        asset.width = inspected.width;
        asset.height = inspected.height;
        if (!inspected.make.empty())
        {
            asset.capture.camera_make = inspected.make;
        }
        if (!inspected.model.empty())
        {
            asset.capture.camera_model = inspected.model;
        }
        asset.capture.iso = inspected.iso;
        asset.capture.aperture = inspected.aperture;
        asset.capture.focal_length_mm = inspected.focal_length_mm;
        asset.capture.shutter_s = inspected.shutter_s;
        asset.capture.captured_unix_s = inspected.captured_unix_s;
        return {};
    };
    const auto map_raw_probe_error = [&](const TaskError &error) -> ImportItemResult
    {
        if (error.code == ErrorCode::kUnsupported || error.code == ErrorCode::kValidation)
        {
            return unsupported_item(location.value().path, error);
        }
        return failed_item(location.value().path, error);
    };

    if (is_raw_extension(file_path))
    {
        auto probed = engine_->inspect_with_embedded_preview(location.value().path,
                                                             kThumbnailMaxEdge, cancellation);
        if (!probed)
        {
            return map_raw_probe_error(probed.error());
        }
        auto applied = apply_inspection(probed.value().inspection);
        if (!applied)
        {
            return unsupported_item(location.value().path, applied.error());
        }
        embedded_preview = std::move(probed.value().embedded_preview);
    }
    else
    {
        auto raster = raster_->probe(location.value().path);
        if (raster)
        {
            asset.media_type = raster.value().media_type;
            asset.width = raster.value().width;
            asset.height = raster.value().height;
            if (is_common_raster_media(asset.media_type))
            {
                auto decoded =
                    raster_->decode(location.value().path, kThumbnailMaxEdge, cancellation);
                if (!decoded)
                {
                    if (decoded.error().code == ErrorCode::kUnsupported)
                    {
                        return unsupported_item(location.value().path, decoded.error());
                    }
                    return failed_item(location.value().path, decoded.error());
                }
                validated_raster = std::move(decoded).value();
            }
        }
        else if (should_try_raw_after_raster(raster.error()))
        {
            auto probed = engine_->inspect_with_embedded_preview(location.value().path,
                                                                 kThumbnailMaxEdge, cancellation);
            if (!probed)
            {
                return map_raw_probe_error(probed.error());
            }
            auto applied = apply_inspection(probed.value().inspection);
            if (!applied)
            {
                return unsupported_item(location.value().path, applied.error());
            }
            embedded_preview = std::move(probed.value().embedded_preview);
        }
        else if (raster.error().code == ErrorCode::kUnsupported)
        {
            return unsupported_item(location.value().path, raster.error());
        }
        else
        {
            return failed_item(location.value().path, raster.error());
        }
    }

    if (is_raw_media_type(asset.media_type) && !embedded_preview)
    {
        auto decoded = engine_->decode_raw_frame(location.value().path, cancellation);
        if (!decoded)
        {
            return map_raw_probe_error(decoded.error());
        }
    }

    if (media_type_has_embedded_capture(asset.media_type))
    {
        auto extracted =
            engine_->read_embedded_capture_metadata(location.value().path, cancellation);
        if (!extracted)
        {
            return failed_item(location.value().path, extracted.error());
        }
        merge_engine_capture(asset.capture, extracted.value());
        auto valid_capture = validate_capture_metadata(asset.capture);
        if (!valid_capture)
        {
            return failed_item(location.value().path, valid_capture.error());
        }
    }

    if (testing_before_import_publication_)
    {
        auto callback = std::move(testing_before_import_publication_);
        callback();
    }
    auto ready_to_publish = cancellation.check();
    if (!ready_to_publish)
    {
        return failed_item(location.value().path, ready_to_publish.error());
    }
    const auto published = repository_->commit_imported_asset(asset);
    if (!published)
    {
        return failed_item(location.value().path, published.error());
    }

    if (validated_raster && preview_policy == ImportPreviewPolicy::kMinimal)
    {
        RasterBuffer raster;
        raster.width = validated_raster->width;
        raster.height = validated_raster->height;
        raster.source_width = validated_raster->source_width;
        raster.source_height = validated_raster->source_height;
        raster.srgb = std::move(validated_raster->rgb);
        raster.color_profile = std::move(validated_raster->color_profile);
        browse_decoded_preview_source_ =
            DecodedPreviewSource{asset.id, asset.content_fingerprint.value_or("none"),
                                 kThumbnailMaxEdge, std::move(raster)};
    }

    Result<PreviewResult> preview = make_error(ErrorCode::kIo, "Preview was not generated");
    if (!defer_preview)
    {
        const std::uint32_t preview_edge =
            preview_policy == ImportPreviewPolicy::kMinimal  ? kThumbnailMaxEdge :
            preview_policy == ImportPreviewPolicy::kStandard ? kDefaultPreviewMaxEdge :
                                                               0U;
        PreviewRequest imported_preview;
        imported_preview.max_edge = preview_edge;
        imported_preview.purpose = PreviewPurpose::kBrowse;
        imported_preview.prefer_embedded_preview =
            preview_policy == ImportPreviewPolicy::kMinimal && is_raw_media_type(asset.media_type);
        imported_preview.cancellation = cancellation;
        if (embedded_preview && preview_policy == ImportPreviewPolicy::kMinimal)
            preview = persist_embedded_browse_preview(asset, *embedded_preview, preview_edge,
                                                      cancellation);
        if (!preview)
            preview = generate_preview(asset, imported_preview, {});
        if (!preview)
        {
            LOG_ERROR(ravo::logger(), "preview failed asset={} path={} error={}", asset.id,
                      location.value().path, preview.error().message);
            PreviewRecord failed;
            failed.asset_id = asset.id;
            failed.state = std::string(kPreviewStateFailed);
            failed.cache_key =
                make_preview_cache_key(asset.id, asset.width.value_or(0), asset.height.value_or(0),
                                       asset.content_fingerprint.value_or("none"));
            static_cast<void>(repository_->upsert_preview(failed));
        }
        else
        {
            LOG_INFO(ravo::logger(), "preview ready asset={} cache={}", asset.id,
                     preview.value().cache_path);
        }
    }

    ImportItemResult result;
    result.status = ImportItemStatus::kImported;
    result.input_path = location.value().path;
    result.asset = asset;
    if (preview)
    {
        result.preview_cache_path = preview.value().cache_path;
    }
    result.preview_pending = defer_preview;
    auto recovered = synchronize_committed_change(asset.id, cancellation);
    if (!recovered)
    {
        result.error = recovered.error();
    }
    return result;
}

Result<std::vector<std::string>>
CatalogService::enumerate_import_inputs(const std::vector<std::string> &paths,
                                        const CancellationToken &cancellation,
                                        const bool recursive) const
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return collect_import_paths(paths, cancellation, recursive);
}

Result<std::vector<ImportItemResult>> CatalogService::import_inputs(
    const std::vector<std::string> &paths, const CancellationToken &cancellation,
    const std::function<void(std::size_t, std::size_t, const ImportItemResult *)> &progress)
{
    auto files = collect_import_paths(paths, cancellation);
    if (!files)
    {
        return files.error();
    }
    std::vector<ImportItemResult> results;
    results.reserve(files.value().size());
    if (files.value().empty())
    {
        if (progress)
        {
            progress(0, 0, nullptr);
        }
        return results;
    }
    const auto started = std::chrono::steady_clock::now();
    if (progress)
    {
        progress(0, files.value().size(), nullptr);
    }
    int imported_count = 0;
    int duplicate_count = 0;
    int unsupported_count = 0;
    int failed_count = 0;
    for (std::size_t index = 0; index < files.value().size(); ++index)
    {
        auto cancelled = cancellation.check();
        if (!cancelled)
        {
            ImportItemResult stopped;
            stopped.status = ImportItemStatus::kFailed;
            stopped.input_path = files.value()[index];
            stopped.error = cancelled.error();
            results.push_back(std::move(stopped));
            ++failed_count;
            break;
        }
        auto item = import_one(files.value()[index], cancellation);
        if (!item)
        {
            results.push_back(failed_item(files.value()[index], item.error()));
            ++failed_count;
        }
        else
        {
            switch (item.value().status)
            {
            case ImportItemStatus::kImported:
                ++imported_count;
                break;
            case ImportItemStatus::kDuplicate:
                ++duplicate_count;
                break;
            case ImportItemStatus::kUnsupported:
                ++unsupported_count;
                break;
            case ImportItemStatus::kFailed:
                ++failed_count;
                break;
            }
            results.push_back(std::move(item).value());
        }
        if (progress)
        {
            progress(index + 1U, files.value().size(), &results.back());
        }
    }
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started)
                                .count();
    LOG_INFO(ravo::logger(),
             "import batch files={} imported={} duplicate={} unsupported={} failed={} {}ms",
             files.value().size(), imported_count, duplicate_count, unsupported_count, failed_count,
             elapsed_ms);
    return results;
}

Result<std::vector<ExportResult>> CatalogService::export_assets(
    const ExportBatchRequest &request,
    const std::function<void(std::size_t, std::size_t, const ExportResult *)> &progress)
{
    if (engine_ == nullptr || raster_ == nullptr || repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto cancelled = request.cancellation.check();
    if (!cancelled)
        return cancelled.error();
    if (request.asset_ids.empty() || request.asset_ids.size() > kExportBatchMaxAssets)
    {
        return make_error(ErrorCode::kInvalidArgument, "Export batch size is invalid",
                          {{"asset_count", std::to_string(request.asset_ids.size())},
                           {"max_assets", std::to_string(kExportBatchMaxAssets)},
                           {"reason", "invalid_export_batch_size"}});
    }
    if (request.output_directory.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "Export batch requires an output directory");
    }
    Result<void> valid_options;
    switch (request.options.format)
    {
    case ExportFormat::kJpeg:
        valid_options = validate_jpeg_export_options(request.options.jpeg_options);
        break;
    case ExportFormat::kPng:
        valid_options = validate_png_export_options(request.options.png_options);
        break;
    case ExportFormat::kTiff:
        valid_options = validate_tiff_export_options(request.options.tiff_options);
        break;
    case ExportFormat::kOriginalCopy:
        if (request.options.metadata_mode != ExportMetadataMode::kFull)
        {
            return make_error(ErrorCode::kValidation,
                              "Metadata privacy mode does not apply to original copy",
                              {{"format", "original"}, {"reason", "metadata_mode_not_applicable"}});
        }
        break;
    }
    if (!valid_options)
        return valid_options.error();

    auto normalized_root = normalize_local_input(request.output_directory);
    if (!normalized_root)
        return normalized_root.error();
    const auto root_path = utf8_path(normalized_root.value().path);
    std::error_code root_error;
    const bool root_is_directory = std::filesystem::is_directory(root_path, root_error);
    if (root_error)
    {
        return make_error(
            ErrorCode::kIo, "Unable to inspect export output directory",
            {{"path", normalized_root.value().path}, {"detail", root_error.message()}});
    }
    if (!root_is_directory)
    {
        return make_error(ErrorCode::kInvalidArgument,
                          "Export output directory does not exist or is not a directory",
                          {{"path", normalized_root.value().path},
                           {"reason", "invalid_export_output_directory"}});
    }

    struct PlannedExport
    {
        std::string asset_id;
        std::string output_path;
    };
    std::vector<PlannedExport> planned;
    planned.reserve(request.asset_ids.size());
    std::set<std::string, std::less<>> unique_assets;
    std::set<std::string, std::less<>> unique_outputs;
    for (std::size_t index = 0; index < request.asset_ids.size(); ++index)
    {
        cancelled = request.cancellation.check();
        if (!cancelled)
        {
            return annotate_batch_export_error(cancelled.error(), 0, request.asset_ids.size(),
                                               index, request.asset_ids[index], {});
        }
        const auto &asset_id = request.asset_ids[index];
        if (asset_id.empty() || !unique_assets.emplace(asset_id).second)
        {
            return make_error(ErrorCode::kValidation,
                              "Export batch asset IDs must be nonempty and unique",
                              {{"asset_id", asset_id},
                               {"batch_index", std::to_string(index + 1U)},
                               {"reason", "duplicate_export_asset_id"}});
        }
        auto asset = repository_->find_asset_by_id(asset_id);
        if (!asset)
            return asset.error();
        if (!asset.value())
        {
            return make_error(
                ErrorCode::kNotFound, "Asset does not exist",
                {{"asset_id", asset_id}, {"batch_index", std::to_string(index + 1U)}});
        }
        auto source = normalize_local_input(asset.value()->normalized_uri);
        if (!source)
            return source.error();
        const auto source_path = utf8_path(source.value().path);
        std::error_code source_error;
        const bool source_is_file = std::filesystem::is_regular_file(source_path, source_error);
        if (source_error)
        {
            return make_error(ErrorCode::kIo, "Unable to inspect export source",
                              {{"asset_id", asset_id},
                               {"batch_index", std::to_string(index + 1U)},
                               {"path", source.value().path},
                               {"detail", source_error.message()}});
        }
        if (!source_is_file)
        {
            return make_error(ErrorCode::kNotFound, "Original file is missing",
                              {{"asset_id", asset_id},
                               {"batch_index", std::to_string(index + 1U)},
                               {"path", source.value().path}});
        }
        const auto stem = utf8_string(source_path.stem().u8string());
        const auto extension = request.options.format == ExportFormat::kOriginalCopy ?
                                   utf8_string(source_path.extension().u8string()) :
                                   std::string(export_format_extension(request.options.format));
        auto filename = expand_export_filename_template(request.filename_template, stem, asset_id,
                                                        index + 1U, extension);
        if (!filename)
        {
            auto error = filename.error();
            error.context.insert_or_assign("asset_id", asset_id);
            error.context.insert_or_assign("batch_index", std::to_string(index + 1U));
            return error;
        }
        const auto output_path = root_path / utf8_path(filename.value());
        const auto output = utf8_string(output_path.generic_u8string());
        if (!unique_outputs.emplace(output).second)
        {
            return make_error(ErrorCode::kConflict,
                              "Export filename template resolves multiple assets to one output",
                              {{"asset_id", asset_id},
                               {"batch_index", std::to_string(index + 1U)},
                               {"output", output},
                               {"reason", "duplicate_export_output"}});
        }
        std::error_code target_error;
        const auto target_status = std::filesystem::symlink_status(output_path, target_error);
        if (target_error && target_error != std::errc::no_such_file_or_directory)
        {
            return make_error(ErrorCode::kIo, "Unable to inspect export output path",
                              {{"asset_id", asset_id},
                               {"batch_index", std::to_string(index + 1U)},
                               {"output", output},
                               {"detail", target_error.message()}});
        }
        if (!target_error && std::filesystem::exists(target_status))
        {
            return make_error(ErrorCode::kConflict, "Export output already exists",
                              {{"asset_id", asset_id},
                               {"batch_index", std::to_string(index + 1U)},
                               {"completed_count", "0"},
                               {"output", output},
                               {"partial_batch", "false"},
                               {"reason", "export_batch_preflight_conflict"},
                               {"total_count", std::to_string(request.asset_ids.size())}});
        }
        planned.push_back({asset_id, output});
    }

    std::vector<ExportResult> results;
    results.reserve(planned.size());
    for (std::size_t index = 0; index < planned.size(); ++index)
    {
        cancelled = request.cancellation.check();
        if (!cancelled)
        {
            return annotate_batch_export_error(cancelled.error(), results.size(), planned.size(),
                                               index, planned[index].asset_id,
                                               planned[index].output_path);
        }
        ExportRequest item;
        static_cast<ExportOptions &>(item) = request.options;
        item.asset_id = planned[index].asset_id;
        item.output_path = planned[index].output_path;
        item.cancellation = request.cancellation;
        item.correlation_id = request.correlation_id.empty() ?
                                  planned[index].asset_id :
                                  request.correlation_id + ":" + std::to_string(index + 1U);
        auto exported = export_asset(item);
        if (!exported)
        {
            return annotate_batch_export_error(exported.error(), results.size(), planned.size(),
                                               index, planned[index].asset_id,
                                               planned[index].output_path);
        }
        results.push_back(std::move(exported).value());
        if (progress)
            progress(index + 1U, planned.size(), &results.back());
    }
    return results;
}

Result<ExportResult> CatalogService::export_asset(const ExportRequest &request)
{
    if (engine_ == nullptr || raster_ == nullptr || repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto cancelled = request.cancellation.check();
    if (!cancelled)
    {
        return cancelled.error();
    }
    if (request.asset_id.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "Export requires an asset ID");
    }
    if (request.output_path.empty())
    {
        return make_error(ErrorCode::kInvalidArgument, "Export requires an output path");
    }
    if (request.format == ExportFormat::kJpeg)
    {
        auto options = validate_jpeg_export_options(request.jpeg_options);
        if (!options)
        {
            return options.error();
        }
    }
    if (request.format == ExportFormat::kPng)
    {
        auto options = validate_png_export_options(request.png_options);
        if (!options)
        {
            return options.error();
        }
    }
    if (request.format == ExportFormat::kTiff)
    {
        auto options = validate_tiff_export_options(request.tiff_options);
        if (!options)
        {
            return options.error();
        }
    }
    if (request.format == ExportFormat::kOriginalCopy &&
        request.metadata_mode != ExportMetadataMode::kFull)
    {
        return make_error(ErrorCode::kValidation,
                          "Metadata privacy mode does not apply to original copy",
                          {{"format", "original"}, {"reason", "metadata_mode_not_applicable"}});
    }
    auto output = normalize_local_input(request.output_path);
    if (!output)
    {
        return output.error();
    }
    auto asset = repository_->find_asset_by_id(request.asset_id);
    if (!asset)
    {
        return asset.error();
    }
    if (!asset.value())
    {
        return make_error(ErrorCode::kNotFound, "Asset does not exist",
                          {{"asset_id", request.asset_id}});
    }
    ExportMetadataSnapshot export_metadata;
    if (request.format != ExportFormat::kOriginalCopy)
    {
        if (request.metadata_mode == ExportMetadataMode::kNone)
        {
            export_metadata.embed_metadata = false;
        }
        else
        {
            if (request.format == ExportFormat::kTiff)
            {
                export_metadata.destination_document_name = output.value().path;
            }
            export_metadata.writable = asset.value()->metadata;
            export_metadata.capture = asset.value()->capture;
            if (request.metadata_mode == ExportMetadataMode::kNoLocation)
            {
                export_metadata.capture.location.reset();
            }
            auto tags = canonicalize_export_tags(asset.value()->tags, request.cancellation);
            if (!tags)
            {
                return tags.error();
            }
            export_metadata.tags = std::move(tags).value();
        }
        auto valid_metadata =
            request.format == ExportFormat::kTiff ?
                validate_tiff_export_metadata(export_metadata, request.cancellation) :
                validate_export_metadata(export_metadata, request.cancellation);
        if (!valid_metadata)
        {
            return valid_metadata.error();
        }
    }
    auto location = normalize_local_input(asset.value()->normalized_uri);
    if (!location)
    {
        return location.error();
    }

    ExportResult result;
    result.asset_id = request.asset_id;
    result.output_path = output.value().path;
    result.format = request.format;
    if (request.format == ExportFormat::kOriginalCopy)
    {
        auto copied =
            copy_file_atomically(location.value().path, output.value().path, request.cancellation);
        if (!copied)
        {
            return copied.error();
        }
        result.width = asset.value()->width.value_or(0);
        result.height = asset.value()->height.value_or(0);
        result.bytes_written = copied.value();
        LOG_INFO(ravo::logger(), "export original asset={} output={} bytes={}", request.asset_id,
                 output.value().path, result.bytes_written);
        return result;
    }

    std::error_code exists_error;
    const bool original_exists =
        std::filesystem::is_regular_file(utf8_path(location.value().path), exists_error) &&
        !exists_error;
    if (!original_exists)
    {
        return make_error(ErrorCode::kNotFound, "Original file is missing",
                          {{"asset_id", request.asset_id}, {"path", location.value().path}});
    }

    auto baseline_recipe = baseline_recipe_for(*asset.value(), location.value().path);
    if (!baseline_recipe)
    {
        return baseline_recipe.error();
    }
    Recipe edit_recipe = std::move(baseline_recipe).value();
    auto stored = repository_->load_recipe_json(request.asset_id);
    if (!stored)
    {
        return stored.error();
    }
    if (stored.value())
    {
        auto parsed = parse_recipe_json(*stored.value());
        if (!parsed)
        {
            return parsed.error();
        }
        parsed.value().asset = edit_recipe.asset;
        auto valid = engine_->validate(parsed.value());
        if (!valid)
        {
            return valid.error();
        }
        edit_recipe = std::move(parsed).value();
    }
    const RenderSampleKind sample_kind = [&request]()
    {
        if (request.format == ExportFormat::kPng &&
            request.png_options.bit_depth == PngBitDepth::k16)
        {
            return RenderSampleKind::kRgb16;
        }
        if (request.format == ExportFormat::kTiff)
        {
            switch (request.tiff_options.sample_type)
            {
            case TiffSampleType::kUint16:
                return RenderSampleKind::kRgb16;
            case TiffSampleType::kFloat16:
            case TiffSampleType::kFloat32:
                return RenderSampleKind::kRgbFloat;
            case TiffSampleType::kUint8:
                break;
            }
        }
        return RenderSampleKind::kRgb8;
    }();
    auto rendered = render_for_export(*asset.value(), location.value().path, edit_recipe,
                                      request.max_edge, request.cancellation, sample_kind);
    if (!rendered)
    {
        return rendered.error();
    }
    ExportPixelBuffer pixels;
    pixels.width = rendered.value().width;
    pixels.height = rendered.value().height;
    pixels.color_profile = std::move(rendered.value().color_profile);
    pixels.samples = std::move(rendered.value().samples);
    auto encoded =
        raster_->encode(pixels, request.format, request.jpeg_options, request.cancellation,
                        request.png_options, request.tiff_options, export_metadata);
    if (!encoded)
    {
        return encoded.error();
    }
    auto written =
        write_bytes_atomically(output.value().path, encoded.value(), request.cancellation);
    if (!written)
    {
        return written.error();
    }
    result.width = pixels.width;
    result.height = pixels.height;
    result.bytes_written = encoded.value().size();
    LOG_INFO(ravo::logger(), "export asset={} format={} output={} {}x{} bytes={}", request.asset_id,
             export_format_name(request.format), output.value().path, result.width, result.height,
             result.bytes_written);
    return result;
}

} // namespace ravo
