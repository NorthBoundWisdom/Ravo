#include "ravo/services/library_service.h"

#include "ravo/services/catalog_service.h"

#include <filesystem>
#include <utility>

#include "ravo/domain/types.h"
#include "ravo/domain/uri.h"

namespace ravo
{

LibraryService::LibraryService(CatalogService &catalog) noexcept
    : catalog_(&catalog)
{
}

Result<CatalogSnapshot> LibraryService::snapshot() const
{
    if (catalog_->repository_ == nullptr || catalog_->cache_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    auto snapshot = catalog_->repository_->snapshot();
    if (!snapshot)
    {
        return snapshot.error();
    }
    snapshot.value().cache_root = catalog_->cache_->root();
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
    if (catalog_->repository_ == nullptr)
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
    if (catalog_->repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto valid = validate_library_page_request(request);
    if (!valid)
        return valid.error();
    LibraryPageRequest expanded = request;
    if (!request.query.collection_id.empty())
    {
        auto set = catalog_->repository_->find_library_set(request.query.collection_id);
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
    return catalog_->repository_->list_assets_page(expanded);
}

Result<std::vector<FolderRecord>> LibraryService::list_folders() const
{
    if (catalog_->repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    auto folders = catalog_->repository_->list_folders();
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
    if (catalog_->repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return catalog_->repository_->list_library_sets();
}

Result<std::optional<LibrarySetRecord>>
LibraryService::find_library_set(const std::string_view set_id) const
{
    if (catalog_->repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return catalog_->repository_->find_library_set(set_id);
}

Result<LibrarySetMutation>
LibraryService::create_library_set(const LibrarySetKind kind, const std::string_view name,
                                   const std::optional<LibraryQuery> &query,
                                   const std::vector<std::string> &asset_ids,
                                   const std::optional<std::int64_t> expected_revision)
{
    return catalog_->create_library_set(kind, name, query, asset_ids, expected_revision);
}

Result<LibrarySetMutation>
LibraryService::rename_library_set(const std::string_view set_id, const std::string_view name,
                                   const std::optional<std::int64_t> expected_revision)
{
    return catalog_->rename_library_set(set_id, name, expected_revision);
}

Result<std::int64_t>
LibraryService::delete_library_set(const std::string_view set_id,
                                   const std::optional<std::int64_t> expected_revision)
{
    return catalog_->delete_library_set(set_id, expected_revision);
}

Result<LibrarySetMutation>
LibraryService::add_library_set_members(const std::string_view set_id,
                                        const std::vector<std::string> &asset_ids,
                                        const std::optional<std::int64_t> expected_revision)
{
    return catalog_->add_library_set_members(set_id, asset_ids, expected_revision);
}

Result<LibrarySetMutation>
LibraryService::remove_library_set_members(const std::string_view set_id,
                                           const std::vector<std::string> &asset_ids,
                                           const std::optional<std::int64_t> expected_revision)
{
    return catalog_->remove_library_set_members(set_id, asset_ids, expected_revision);
}

Result<AssetVersionMutation>
LibraryService::create_asset_version(const std::string_view source_asset_id,
                                     const std::optional<std::int64_t> expected_revision)
{
    return catalog_->create_asset_version(source_asset_id, expected_revision);
}

Result<LibraryStackMutation>
LibraryService::stack_assets(const std::vector<std::string> &asset_ids,
                             const std::string_view pick_asset_id,
                             const std::optional<std::int64_t> expected_revision)
{
    return catalog_->stack_assets(asset_ids, pick_asset_id, expected_revision);
}

Result<std::int64_t>
LibraryService::unstack_assets(const std::string_view stack_id,
                               const std::optional<std::int64_t> expected_revision)
{
    return catalog_->unstack_assets(stack_id, expected_revision);
}

Result<LibraryStackMutation>
LibraryService::set_stack_pick(const std::string_view stack_id,
                               const std::string_view pick_asset_id,
                               const std::optional<std::int64_t> expected_revision)
{
    return catalog_->set_stack_pick(stack_id, pick_asset_id, expected_revision);
}

Result<std::optional<LibraryStackRecord>>
LibraryService::find_library_stack(const std::string_view stack_id) const
{
    return catalog_->find_library_stack(stack_id);
}

Result<FolderRelinkResult>
LibraryService::relink_folder(const std::string_view folder_id,
                              const std::string_view replacement_directory,
                              const CancellationToken &cancellation)
{
    return catalog_->relink_folder(folder_id, replacement_directory, cancellation);
}

Result<FolderRemoveResult>
LibraryService::remove_folder_from_catalog(const std::string_view folder_uri,
                                           const CancellationToken &cancellation)
{
    return catalog_->remove_folder_from_catalog(folder_uri, cancellation);
}

Result<std::vector<PreviewRecord>> LibraryService::list_previews() const
{
    if (catalog_->repository_ == nullptr)
    {
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    }
    return catalog_->repository_->list_previews();
}

Result<std::vector<PreviewRecord>>
LibraryService::list_previews_for_assets(const std::vector<std::string> &asset_ids) const
{
    if (catalog_->repository_ == nullptr)
        return make_error(ErrorCode::kIo, "Catalog session is closed");
    return catalog_->repository_->list_previews_for_assets(asset_ids);
}

Result<AssetRecord> LibraryService::set_rating(const std::string_view asset_id, const int rating)
{
    return catalog_->set_rating(asset_id, rating);
}

Result<AssetRecord> LibraryService::set_color_label(const std::string_view asset_id,
                                                    const ColorLabel label)
{
    return catalog_->set_color_label(asset_id, label);
}

Result<AssetRecord> LibraryService::set_rejected(const std::string_view asset_id,
                                                 const bool rejected)
{
    return catalog_->set_rejected(asset_id, rejected);
}

Result<AssetRecord> LibraryService::set_picked(const std::string_view asset_id, const bool picked)
{
    return catalog_->set_picked(asset_id, picked);
}

Result<void> LibraryService::remove_from_catalog(const std::string_view asset_id)
{
    return catalog_->remove_from_catalog(asset_id);
}

Result<void> LibraryService::remove_original_and_catalog(const std::string_view asset_id)
{
    return catalog_->remove_original_and_catalog(asset_id);
}

} // namespace ravo
