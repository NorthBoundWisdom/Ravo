#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo
{

class CatalogService;

// Narrow LibraryService capability face over CatalogService.
class LibraryService
{
public:
    explicit LibraryService(CatalogService &catalog) noexcept;

    LibraryService(const LibraryService &) = delete;
    LibraryService &operator=(const LibraryService &) = delete;
    LibraryService(LibraryService &&) noexcept = default;
    LibraryService &operator=(LibraryService &&) noexcept = default;

    [[nodiscard]] Result<CatalogSnapshot> snapshot() const;
    [[nodiscard]] Result<std::vector<AssetRecord>> list_assets() const;
    [[nodiscard]] Result<std::vector<AssetRecord>> list_assets(const LibraryQuery &query) const;
    [[nodiscard]] Result<std::vector<AssetRecord>> list_assets(const LibraryQuery &query,
                                                               bool collapse_stacks) const;
    [[nodiscard]] Result<LibraryPage> list_assets_page(const LibraryPageRequest &request) const;
    [[nodiscard]] Result<std::vector<FolderRecord>> list_folders() const;
    [[nodiscard]] Result<std::vector<LibrarySetRecord>> list_library_sets() const;
    [[nodiscard]] Result<std::optional<LibrarySetRecord>>
    find_library_set(std::string_view set_id) const;
    [[nodiscard]] Result<LibrarySetMutation>
    create_library_set(LibrarySetKind kind, std::string_view name,
                       const std::optional<LibraryQuery> &query,
                       const std::vector<std::string> &asset_ids,
                       std::optional<std::int64_t> expected_revision = {});
    [[nodiscard]] Result<LibrarySetMutation>
    rename_library_set(std::string_view set_id, std::string_view name,
                       std::optional<std::int64_t> expected_revision = {});
    [[nodiscard]] Result<std::int64_t>
    delete_library_set(std::string_view set_id, std::optional<std::int64_t> expected_revision = {});
    [[nodiscard]] Result<LibrarySetMutation>
    add_library_set_members(std::string_view set_id, const std::vector<std::string> &asset_ids,
                            std::optional<std::int64_t> expected_revision = {});
    [[nodiscard]] Result<LibrarySetMutation>
    remove_library_set_members(std::string_view set_id, const std::vector<std::string> &asset_ids,
                               std::optional<std::int64_t> expected_revision = {});
    [[nodiscard]] Result<AssetVersionMutation>
    create_asset_version(std::string_view source_asset_id,
                         std::optional<std::int64_t> expected_revision = {});
    [[nodiscard]] Result<LibraryStackMutation>
    stack_assets(const std::vector<std::string> &asset_ids, std::string_view pick_asset_id,
                 std::optional<std::int64_t> expected_revision = {});
    [[nodiscard]] Result<std::int64_t>
    unstack_assets(std::string_view stack_id, std::optional<std::int64_t> expected_revision = {});
    [[nodiscard]] Result<LibraryStackMutation>
    set_stack_pick(std::string_view stack_id, std::string_view pick_asset_id,
                   std::optional<std::int64_t> expected_revision = {});
    [[nodiscard]] Result<std::optional<LibraryStackRecord>>
    find_library_stack(std::string_view stack_id) const;
    [[nodiscard]] Result<FolderRelinkResult>
    relink_folder(std::string_view folder_id, std::string_view replacement_directory,
                  const CancellationToken &cancellation = {});
    [[nodiscard]] Result<FolderRemoveResult>
    remove_folder_from_catalog(std::string_view folder_uri,
                               const CancellationToken &cancellation = {});
    [[nodiscard]] Result<std::vector<PreviewRecord>> list_previews() const;
    [[nodiscard]] Result<std::vector<PreviewRecord>>
    list_previews_for_assets(const std::vector<std::string> &asset_ids) const;
    [[nodiscard]] Result<AssetRecord> set_rating(std::string_view asset_id, int rating);
    [[nodiscard]] Result<AssetRecord> set_color_label(std::string_view asset_id, ColorLabel label);
    [[nodiscard]] Result<AssetRecord> set_rejected(std::string_view asset_id, bool rejected);
    [[nodiscard]] Result<AssetRecord> set_picked(std::string_view asset_id, bool picked);
    [[nodiscard]] Result<void> remove_from_catalog(std::string_view asset_id);
    [[nodiscard]] Result<void> remove_original_and_catalog(std::string_view asset_id);

private:
    CatalogService *catalog_ = nullptr;
};

} // namespace ravo
