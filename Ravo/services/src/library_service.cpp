#include "ravo/services/library_service.h"

#include <filesystem>
#include <utility>
#include <set>

#include "catalog_internal.h"
#include "catalog_service_internal.h"
#include "ravo/domain/catalog_repository.h"
#include "ravo/domain/uri.h"
#include "ravo/services/recovery_service.h"
#include "ravo/domain/preview_cache.h"
#include "ravo/domain/recovery_store.h"

namespace ravo
{
using namespace catalog_service_internal;
namespace
{

[[nodiscard]] std::filesystem::path path_from_utf8(const std::string_view value)
{
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

[[nodiscard]] std::string path_to_utf8(const std::filesystem::path &path)
{
    const auto value = path.generic_u8string();
    return {reinterpret_cast<const char *>(value.data()), value.size()};
}

} // namespace

LibraryService::LibraryService(const std::unique_ptr<CatalogRepository> &repository,
                               const std::shared_ptr<PreviewCache> &cache,
                               const std::unique_ptr<RecoveryStore> &recovery,
                               RecoveryService &recovery_service) noexcept
    : repository_(repository)
    , cache_(cache)
    , recovery_(recovery)
    , recovery_service_(recovery_service)
{
}

Result<CatalogSnapshot> LibraryService::snapshot() const
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

Result<std::vector<AssetRecord>> LibraryService::list_assets() const
{
    return list_assets(LibraryQuery{}, true);
}

Result<std::vector<AssetRecord>> LibraryService::list_assets(const LibraryQuery &query) const
{
    return list_assets(query, true);
}

Result<std::vector<AssetRecord>> LibraryService::list_assets(const LibraryQuery &query,
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

Result<LibraryPage> LibraryService::list_assets_page(const LibraryPageRequest &request) const
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

Result<std::vector<FolderRecord>> LibraryService::list_folders() const
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

Result<std::vector<LibrarySetRecord>> LibraryService::list_library_sets() const
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return repository_->list_library_sets();
}

Result<std::optional<LibrarySetRecord>>
LibraryService::find_library_set(const std::string_view set_id) const
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return repository_->find_library_set(set_id);
}

Result<std::vector<PreviewRecord>> LibraryService::list_previews() const
{
    if (repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    return repository_->list_previews();
}

Result<std::vector<PreviewRecord>>
LibraryService::list_previews_for_assets(const std::vector<std::string> &asset_ids) const
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return repository_->list_previews_for_assets(asset_ids);
}

Result<LibrarySetMutation>
LibraryService::create_library_set(const LibrarySetKind kind, const std::string_view name,
                                   const std::optional<LibraryQuery> &query,
                                   const std::vector<std::string> &asset_ids,
                                   const std::optional<std::int64_t> expected_revision)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return repository_->create_library_set(kind, name, query, asset_ids, expected_revision);
}

Result<LibrarySetMutation>
LibraryService::rename_library_set(const std::string_view set_id, const std::string_view name,
                                   const std::optional<std::int64_t> expected_revision)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return repository_->rename_library_set(set_id, name, expected_revision);
}

Result<std::int64_t>
LibraryService::delete_library_set(const std::string_view set_id,
                                   const std::optional<std::int64_t> expected_revision)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return repository_->delete_library_set(set_id, expected_revision);
}

Result<LibrarySetMutation>
LibraryService::add_library_set_members(const std::string_view set_id,
                                        const std::vector<std::string> &asset_ids,
                                        const std::optional<std::int64_t> expected_revision)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return repository_->add_library_set_members(set_id, asset_ids, expected_revision);
}

Result<LibrarySetMutation>
LibraryService::remove_library_set_members(const std::string_view set_id,
                                           const std::vector<std::string> &asset_ids,
                                           const std::optional<std::int64_t> expected_revision)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return repository_->remove_library_set_members(set_id, asset_ids, expected_revision);
}

Result<AssetVersionMutation>
LibraryService::create_asset_version(const std::string_view source_asset_id,
                                     const std::optional<std::int64_t> expected_revision)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto source = repository_->find_asset_by_id(source_asset_id);
    if (!source)
        return source.error();
    if (!source.value())
        return make_error(ErrorCode::kNotFound, "Asset does not exist");
    if (is_video_media_type(source.value()->media_type))
        return make_error(ErrorCode::kUnsupported, "Video does not support photo versions",
                          {{"reason", "video_photo_operation_unsupported"}});
    auto created = repository_->create_asset_version(source_asset_id, expected_revision);
    if (!created)
        return created.error();
    auto recovered = recovery_service_.synchronize_committed_change(created.value().version.id);
    if (!recovered)
        return recovered.error();
    return created;
}

Result<LibraryStackMutation>
LibraryService::stack_assets(const std::vector<std::string> &asset_ids,
                             const std::string_view pick_asset_id,
                             const std::optional<std::int64_t> expected_revision)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return repository_->stack_assets(asset_ids, pick_asset_id, expected_revision);
}

Result<std::int64_t>
LibraryService::unstack_assets(const std::string_view stack_id,
                               const std::optional<std::int64_t> expected_revision)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return repository_->unstack_assets(stack_id, expected_revision);
}

Result<LibraryStackMutation>
LibraryService::set_stack_pick(const std::string_view stack_id,
                               const std::string_view pick_asset_id,
                               const std::optional<std::int64_t> expected_revision)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return repository_->set_stack_pick(stack_id, pick_asset_id, expected_revision);
}

Result<std::optional<LibraryStackRecord>>
LibraryService::find_library_stack(const std::string_view stack_id) const
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return repository_->find_library_stack(stack_id);
}

Result<AssetRecord> LibraryService::set_rating(const std::string_view asset_id, const int rating)
{
    ReviewPatch patch;
    patch.rating = rating;
    return apply_review_patch(asset_id, patch);
}

Result<AssetRecord> LibraryService::set_color_label(const std::string_view asset_id,
                                                    const ColorLabel label)
{
    ReviewPatch patch;
    patch.color_label = label;
    return apply_review_patch(asset_id, patch);
}

Result<AssetRecord> LibraryService::set_rejected(const std::string_view asset_id,
                                                 const bool rejected)
{
    ReviewPatch patch;
    patch.rejected = rejected;
    return apply_review_patch(asset_id, patch);
}

Result<void> LibraryService::remove_from_catalog(const std::string_view asset_id)
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

Result<void> LibraryService::remove_original_and_catalog(const std::string_view asset_id)
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

Result<FolderRelinkResult>
LibraryService::relink_folder(const std::string_view folder_id,
                              const std::string_view replacement_directory,
                              const CancellationToken &cancellation)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    if (folder_id.empty() || replacement_directory.empty())
        return make_error(ErrorCode::kInvalidArgument,
                          "Folder relink requires an identity and replacement directory",
                          {{"reason", "missing_folder_relink_input"}});
    auto active = cancellation.check();
    if (!active)
        return active.error();
    auto folder = repository_->find_folder_by_id(folder_id);
    if (!folder)
        return folder.error();
    if (!folder.value())
        return make_error(ErrorCode::kNotFound, "Folder identity does not exist",
                          {{"folder_id", std::string(folder_id)}});

    auto replacement = normalize_local_input(replacement_directory);
    if (!replacement)
        return replacement.error();
    std::error_code replacement_error;
    if (!std::filesystem::is_directory(path_from_utf8(replacement.value().path),
                                       replacement_error) ||
        replacement_error)
        return make_error(ErrorCode::kNotFound, "Replacement folder is not available",
                          {{"reason", "replacement_folder_missing"},
                           {"path", replacement.value().path},
                           {"detail", replacement_error.message()}});
    if (replacement.value().uri == folder.value()->uri)
        return make_error(
            ErrorCode::kConflict, "Replacement folder has not changed",
            {{"reason", "folder_relink_noop"}, {"folder_id", std::string(folder_id)}});

    auto assets = repository_->list_folder_assets(folder_id);
    if (!assets)
        return assets.error();
    if (assets.value().empty() || assets.value().size() > kImportBatchMaximumAssets)
        return make_error(ErrorCode::kValidation, "Folder asset set is invalid",
                          {{"reason", "invalid_folder_asset_set"},
                           {"folder_id", std::string(folder_id)},
                           {"asset_count", std::to_string(assets.value().size())}});

    FolderRelinkCommit commit;
    commit.folder_id = std::string(folder_id);
    commit.expected_old_uri = folder.value()->uri;
    commit.replacement_uri = replacement.value().uri;
    commit.assets.reserve(assets.value().size());
    std::set<std::string, std::less<>> replacement_uris;
    for (const auto &asset : assets.value())
    {
        active = cancellation.check();
        if (!active)
            return active.error();
        auto old_asset = normalize_local_input(asset.normalized_uri);
        if (!old_asset)
            return old_asset.error();
        const auto filename = path_from_utf8(old_asset.value().path).filename();
        if (filename.empty())
            return make_error(
                ErrorCode::kValidation, "Catalog asset filename is invalid",
                {{"reason", "invalid_relink_asset_filename"}, {"asset_id", asset.id}});
        const auto candidate_path = path_from_utf8(replacement.value().path) / filename;
        auto candidate = normalize_local_input(path_to_utf8(candidate_path));
        if (!candidate)
            return candidate.error();
        if (!replacement_uris.insert(candidate.value().uri).second)
            return make_error(
                ErrorCode::kConflict, "Replacement folder maps multiple assets to one path",
                {{"reason", "duplicate_replacement_uri"}, {"uri", candidate.value().uri}});
        auto identity = read_file_identity(candidate.value().path);
        if (!identity)
        {
            auto error = identity.error();
            error.context.insert_or_assign("folder_id", std::string(folder_id));
            error.context.insert_or_assign("asset_id", asset.id);
            error.context.insert_or_assign("replacement_uri", candidate.value().uri);
            error.context.insert_or_assign("reason", "replacement_asset_missing");
            return error;
        }
        if (identity.value().size_bytes != asset.size_bytes ||
            identity.value().mtime_unix_ms != asset.mtime_unix_ms ||
            (asset.content_fingerprint &&
             make_content_fingerprint(identity.value()) != *asset.content_fingerprint))
            return make_error(ErrorCode::kConflict,
                              "Replacement asset identity does not match the catalog",
                              {{"reason", "replacement_asset_identity_mismatch"},
                               {"folder_id", std::string(folder_id)},
                               {"asset_id", asset.id},
                               {"replacement_uri", candidate.value().uri}});
        auto existing = repository_->find_asset_by_uri(candidate.value().uri);
        if (!existing)
            return existing.error();
        if (existing.value() && existing.value()->id != asset.id)
            return make_error(ErrorCode::kConflict, "Replacement asset URI is already cataloged",
                              {{"reason", "asset_uri_conflict"},
                               {"asset_id", asset.id},
                               {"conflicting_asset_id", existing.value()->id},
                               {"replacement_uri", candidate.value().uri}});
        commit.assets.push_back({asset.id, asset.normalized_uri, std::move(candidate).value().uri});
    }
    active = cancellation.check();
    if (!active)
        return active.error();
    auto committed = repository_->commit_folder_relink(commit, cancellation);
    if (!committed)
        return committed.error();

    FolderRelinkResult result;
    result.folder_id = std::string(folder_id);
    result.previous_uri = folder.value()->uri;
    result.replacement_uri = replacement.value().uri;
    result.asset_count = commit.assets.size();
    result.recovery_pending = commit.assets.size();
    return result;
}

Result<FolderRemoveResult>
LibraryService::remove_folder_from_catalog(const std::string_view folder_uri,
                                           const CancellationToken &cancellation)
{
    if (repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    if (folder_uri.empty())
        return make_error(ErrorCode::kInvalidArgument,
                          "All Photographs cannot be removed from the catalog",
                          {{"reason", "all_photographs_not_removable"}});
    auto active = cancellation.check();
    if (!active)
        return active.error();
    LibraryQuery query;
    query.folder_uri = std::string(folder_uri);
    auto listed = list_assets(query, false);
    if (!listed)
        return listed.error();
    if (listed.value().empty())
        return make_error(ErrorCode::kNotFound, "Folder has no cataloged photos",
                          {{"reason", "folder_empty"}, {"folder_uri", std::string(folder_uri)}});
    FolderRemoveResult result;
    result.folder_uri = std::string(folder_uri);
    for (const auto &asset : listed.value())
    {
        active = cancellation.check();
        if (!active)
            return active.error();
        auto removed = remove_from_catalog(asset.id);
        if (!removed)
        {
            if (removed.error().code == ErrorCode::kNotFound)
                continue;
            return removed.error();
        }
        ++result.asset_count;
    }
    if (result.asset_count == 0U)
        return make_error(ErrorCode::kNotFound, "Folder has no cataloged photos",
                          {{"reason", "folder_empty"}, {"folder_uri", std::string(folder_uri)}});
    return result;
}

Result<AssetRecord> LibraryService::set_picked(const std::string_view asset_id, const bool picked)
{
    ReviewPatch patch;
    patch.picked = picked;
    return apply_review_patch(asset_id, patch);
}

Result<AssetRecord> LibraryService::apply_review_patch(const std::string_view asset_id,
                                                       const ReviewPatch &patch)
{
    if (!repository_)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto committed = repository_->commit_review_patch(asset_id, patch);
    if (!committed)
        return committed.error();
    auto recovered = recovery_service_.synchronize_committed_change(asset_id);
    if (!recovered)
        return recovered.error();
    auto asset = repository_->find_asset_by_id(asset_id);
    if (!asset)
        return asset.error();
    if (!asset.value())
        return make_error(ErrorCode::kNotFound, "Committed asset no longer exists");
    return *asset.value();
}

} // namespace ravo
