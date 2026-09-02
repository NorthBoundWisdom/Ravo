#include "ravo/services/catalog_service.h"

#include <filesystem>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ravo/domain/uri.h"

namespace ravo
{
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

Result<FolderRelinkResult>
CatalogService::relink_folder(const std::string_view folder_id,
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

    auto old_location = normalize_local_input(folder.value()->uri);
    if (!old_location)
        return old_location.error();
    std::error_code old_error;
    const bool old_is_directory =
        std::filesystem::is_directory(path_from_utf8(old_location.value().path), old_error);
    if (old_error && old_error != std::errc::no_such_file_or_directory)
        return make_error(ErrorCode::kIo, "Unable to inspect the cataloged folder",
                          {{"reason", "folder_root_inspect_failed"},
                           {"folder_id", std::string(folder_id)},
                           {"uri", folder.value()->uri},
                           {"detail", old_error.message()}});
    if (old_is_directory)
        return make_error(ErrorCode::kConflict, "Folder is still available at its catalog path",
                          {{"reason", "folder_root_not_missing"},
                           {"folder_id", std::string(folder_id)},
                           {"uri", folder.value()->uri}});

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
CatalogService::remove_folder_from_catalog(const std::string_view folder_uri,
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

} // namespace ravo
