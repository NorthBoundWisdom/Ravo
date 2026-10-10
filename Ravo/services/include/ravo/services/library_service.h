#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo
{

class CatalogService;
class CatalogRepository;
class PreviewCache;
class RecoveryStore;
class RecoveryService;

class LibraryService
{
public:
    LibraryService(const LibraryService &) = delete;
    LibraryService &operator=(const LibraryService &) = delete;
    LibraryService(LibraryService &&) = delete;
    LibraryService &operator=(LibraryService &&) = delete;

    [[nodiscard]] Result<CatalogSnapshot> snapshot() const;
    [[nodiscard]] Result<std::vector<AssetRecord>> list_assets() const;
    [[nodiscard]] Result<std::vector<AssetRecord>> list_assets(const LibraryQuery &query) const;
    [[nodiscard]] Result<std::vector<AssetRecord>> list_assets(const LibraryQuery &query,
                                                               bool collapse_stacks) const;
    [[nodiscard]] Result<LibraryPage> list_assets_page(const LibraryPageRequest &request) const;
    // Resolve [offset, offset + count) in the same listing, independently of
    // display residency. A stale revision or cancellation publishes no snapshot.
    [[nodiscard]] Result<std::vector<LibrarySelectionAsset>>
    resolve_selection_ids(LibraryPageRequest request, std::size_t count,
                          std::int64_t expected_revision) const;
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
    [[nodiscard]] Result<AssetRecord> apply_review_patch(std::string_view asset_id,
                                                         const ReviewPatch &patch);
    friend class CatalogService;
    // Borrowed owner slots stay valid until this capability is destroyed. The
    // composition owner is immovable; reset slots make post-close calls fail.
    LibraryService(const std::unique_ptr<CatalogRepository> &repository,
                   const std::shared_ptr<PreviewCache> &cache,
                   const std::unique_ptr<RecoveryStore> &recovery,
                   RecoveryService &recovery_service) noexcept;

    const std::unique_ptr<CatalogRepository> &repository_;
    const std::shared_ptr<PreviewCache> &cache_;
    const std::unique_ptr<RecoveryStore> &recovery_;
    RecoveryService &recovery_service_;
};

} // namespace ravo
