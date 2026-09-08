#include "ravo/services/library_service.h"

#include "ravo/services/catalog_service.h"

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
    return catalog_->list_assets();
}

Result<std::vector<AssetRecord>> LibraryService::list_assets(const LibraryQuery &query) const
{
    return catalog_->list_assets(query);
}

Result<std::vector<AssetRecord>> LibraryService::list_assets(const LibraryQuery &query,
                                                             const bool collapse_stacks) const
{
    return catalog_->list_assets(query, collapse_stacks);
}

Result<LibraryPage> LibraryService::list_assets_page(const LibraryPageRequest &request) const
{
    return catalog_->list_assets_page(request);
}

Result<std::vector<FolderRecord>> LibraryService::list_folders() const
{
    return catalog_->list_folders();
}

Result<std::vector<LibrarySetRecord>> LibraryService::list_library_sets() const
{
    return catalog_->list_library_sets();
}

Result<std::optional<LibrarySetRecord>>
LibraryService::find_library_set(const std::string_view set_id) const
{
    return catalog_->find_library_set(set_id);
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
    return catalog_->list_previews();
}

Result<std::vector<PreviewRecord>>
LibraryService::list_previews_for_assets(const std::vector<std::string> &asset_ids) const
{
    return catalog_->list_previews_for_assets(asset_ids);
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
