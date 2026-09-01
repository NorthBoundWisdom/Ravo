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
#include "catalog_service_internal.h"
#include "catalog_service_test_support.h"
#include "ravo/domain/uri.h"
#include "ravo/foundation/log.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"

namespace ravo
{
using namespace catalog_service_internal;

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

void testing::CatalogServiceTestControl::set_import_checkpoint(
    CatalogService &service,
    std::function<Result<void>(std::string_view checkpoint, std::string_view path)> callback)
{
    service.testing_import_checkpoint_ = std::move(callback);
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
    return list_assets(LibraryQuery{}, true);
}

Result<std::vector<AssetRecord>> CatalogService::list_assets(const LibraryQuery &query) const
{
    return list_assets(query, true);
}

Result<std::vector<AssetRecord>> CatalogService::list_assets(const LibraryQuery &query,
                                                             const bool collapse_stacks) const
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
    std::vector<AssetRecord> assets;
    LibraryPageRequest page_request;
    page_request.query = query;
    page_request.collapse_stacks = collapse_stacks;
    page_request.limit = kLibraryPageMaximumSize;
    while (true)
    {
        auto page = list_assets_page(page_request);
        if (!page)
            return page.error();
        assets.insert(assets.end(), page.value().assets.begin(), page.value().assets.end());
        if (!page.value().has_more || page.value().assets.empty())
            break;
        page_request.offset += page.value().assets.size();
        page_request.known_total = page.value().total;
        page_request.after_asset_id = page.value().assets.back().id;
    }
    return assets;
}

Result<LibraryPage> CatalogService::list_assets_page(const LibraryPageRequest &request) const
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto valid = validate_library_page_request(request);
    if (!valid)
        return valid.error();
    LibraryPageRequest expanded = request;
    if (!request.query.collection_id.empty())
    {
        auto set = repository_->find_library_set(request.query.collection_id);
        if (!set)
            return set.error();
        if (!set.value())
        {
            return make_error(
                ErrorCode::kNotFound, "Library set was not found",
                {{"set_id", request.query.collection_id}, {"reason", "unknown_library_set"}});
        }
        if (set.value()->kind == LibrarySetKind::kSmart)
        {
            if (!set.value()->query)
            {
                return make_error(ErrorCode::kValidation, "A smart library set requires a query",
                                  {{"reason", "invalid_library_set_query"}});
            }
            LibraryQuery session = request.query;
            session.collection_id.clear();
            expanded.query = *set.value()->query;
            expanded.query.sort_field = request.query.sort_field;
            expanded.query.sort_direction = request.query.sort_direction;
            expanded.additional_query = std::move(session);
            auto extra_valid = validate_library_page_request(expanded);
            if (!extra_valid)
                return extra_valid.error();
        }
    }
    return repository_->list_assets_page(expanded);
}

Result<std::vector<LibrarySetRecord>> CatalogService::list_library_sets() const
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return repository_->list_library_sets();
}

Result<std::optional<LibrarySetRecord>>
CatalogService::find_library_set(const std::string_view set_id) const
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return repository_->find_library_set(set_id);
}

Result<LibrarySetMutation>
CatalogService::create_library_set(const LibrarySetKind kind, const std::string_view name,
                                   const std::optional<LibraryQuery> &query,
                                   const std::vector<std::string> &asset_ids,
                                   const std::optional<std::int64_t> expected_revision)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return repository_->create_library_set(kind, name, query, asset_ids, expected_revision);
}

Result<LibrarySetMutation>
CatalogService::rename_library_set(const std::string_view set_id, const std::string_view name,
                                   const std::optional<std::int64_t> expected_revision)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return repository_->rename_library_set(set_id, name, expected_revision);
}

Result<std::int64_t>
CatalogService::delete_library_set(const std::string_view set_id,
                                   const std::optional<std::int64_t> expected_revision)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return repository_->delete_library_set(set_id, expected_revision);
}

Result<LibrarySetMutation>
CatalogService::add_library_set_members(const std::string_view set_id,
                                        const std::vector<std::string> &asset_ids,
                                        const std::optional<std::int64_t> expected_revision)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return repository_->add_library_set_members(set_id, asset_ids, expected_revision);
}

Result<LibrarySetMutation>
CatalogService::remove_library_set_members(const std::string_view set_id,
                                           const std::vector<std::string> &asset_ids,
                                           const std::optional<std::int64_t> expected_revision)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return repository_->remove_library_set_members(set_id, asset_ids, expected_revision);
}

Result<AssetVersionMutation>
CatalogService::create_asset_version(const std::string_view source_asset_id,
                                     const std::optional<std::int64_t> expected_revision)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto created = repository_->create_asset_version(source_asset_id, expected_revision);
    if (!created)
        return created.error();
    auto recovered = synchronize_committed_change(created.value().version.id);
    if (!recovered)
        return recovered.error();
    return created;
}

Result<LibraryStackMutation>
CatalogService::stack_assets(const std::vector<std::string> &asset_ids,
                             const std::string_view pick_asset_id,
                             const std::optional<std::int64_t> expected_revision)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return repository_->stack_assets(asset_ids, pick_asset_id, expected_revision);
}

Result<std::int64_t>
CatalogService::unstack_assets(const std::string_view stack_id,
                               const std::optional<std::int64_t> expected_revision)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return repository_->unstack_assets(stack_id, expected_revision);
}

Result<LibraryStackMutation>
CatalogService::set_stack_pick(const std::string_view stack_id,
                               const std::string_view pick_asset_id,
                               const std::optional<std::int64_t> expected_revision)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return repository_->set_stack_pick(stack_id, pick_asset_id, expected_revision);
}

Result<std::optional<LibraryStackRecord>>
CatalogService::find_library_stack(const std::string_view stack_id) const
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return repository_->find_library_stack(stack_id);
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
    auto version_ids = repository_->list_version_asset_ids(asset_id);
    if (!version_ids)
        return version_ids.error();
    std::vector<std::string> removed_ids = std::move(version_ids).value();
    removed_ids.push_back(std::string(asset_id));
    if (cache_ != nullptr)
    {
        for (const auto &id : removed_ids)
        {
            const auto removed_cache = cache_->remove_for_asset(id);
            if (!removed_cache)
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
        for (const auto &id : removed_ids)
        {
            auto removed_recovery = recovery_->remove_asset(id);
            if (!removed_recovery)
            {
                auto error = removed_recovery.error();
                error.context.insert_or_assign("asset_id", id);
                error.context.insert_or_assign("catalog_removed", "true");
                return error;
            }
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
    if (asset.value()->version_ordinal != kAssetVersionOrdinalPrimary)
    {
        return make_error(
            ErrorCode::kValidation, "Only a primary asset can delete the original file",
            {{"asset_id", std::string(asset_id)}, {"reason", "version_disk_delete_forbidden"}});
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

} // namespace ravo
