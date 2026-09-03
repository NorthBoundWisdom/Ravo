#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/domain/types.h"
#include "ravo/foundation/error.h"

namespace ravo
{

class CatalogBackupDatabaseVerifier
{
public:
    virtual ~CatalogBackupDatabaseVerifier() = default;

    [[nodiscard]] virtual Result<CatalogDatabaseArtifact>
    verify_backup_database(std::string_view backup_path, std::string_view expected_sha256,
                           const CancellationToken &cancellation) const = 0;
};

class CatalogRestoreDatabaseVerifier
{
public:
    virtual ~CatalogRestoreDatabaseVerifier() = default;

    // Uses the ordinary repository open/schema path after restore publication.
    // Implementations must close all handles before returning the immutable
    // snapshot and reject a catalog identity mismatch.
    [[nodiscard]] virtual Result<CatalogSnapshot>
    verify_restored_catalog(std::string_view catalog_path, std::string_view expected_catalog_id,
                            const CancellationToken &cancellation) const = 0;
};

class CatalogRepository : public CatalogBackupDatabaseVerifier
{
public:
    virtual ~CatalogRepository() = default;

    [[nodiscard]] virtual Result<CatalogSnapshot> snapshot() const = 0;
    [[nodiscard]] virtual Result<std::vector<AssetRecord>> list_assets() const = 0;
    [[nodiscard]] virtual Result<LibraryPage>
    list_assets_page(const LibraryPageRequest &request) const = 0;
    [[nodiscard]] virtual Result<std::vector<FolderRecord>> list_folders() const = 0;
    [[nodiscard]] virtual Result<std::vector<LibrarySetRecord>> list_library_sets() const = 0;
    [[nodiscard]] virtual Result<std::optional<LibrarySetRecord>>
    find_library_set(std::string_view set_id) const = 0;
    [[nodiscard]] virtual Result<LibrarySetMutation>
    create_library_set(LibrarySetKind kind, std::string_view name,
                       const std::optional<LibraryQuery> &query,
                       const std::vector<std::string> &asset_ids,
                       std::optional<std::int64_t> expected_revision) = 0;
    [[nodiscard]] virtual Result<LibrarySetMutation>
    rename_library_set(std::string_view set_id, std::string_view name,
                       std::optional<std::int64_t> expected_revision) = 0;
    [[nodiscard]] virtual Result<std::int64_t>
    delete_library_set(std::string_view set_id, std::optional<std::int64_t> expected_revision) = 0;
    [[nodiscard]] virtual Result<LibrarySetMutation>
    add_library_set_members(std::string_view set_id, const std::vector<std::string> &asset_ids,
                            std::optional<std::int64_t> expected_revision) = 0;
    [[nodiscard]] virtual Result<LibrarySetMutation>
    remove_library_set_members(std::string_view set_id, const std::vector<std::string> &asset_ids,
                               std::optional<std::int64_t> expected_revision) = 0;
    [[nodiscard]] virtual Result<AssetVersionMutation>
    create_asset_version(std::string_view source_asset_id,
                         std::optional<std::int64_t> expected_revision) = 0;
    [[nodiscard]] virtual Result<LibraryStackMutation>
    stack_assets(const std::vector<std::string> &asset_ids, std::string_view pick_asset_id,
                 std::optional<std::int64_t> expected_revision) = 0;
    [[nodiscard]] virtual Result<std::int64_t>
    unstack_assets(std::string_view stack_id, std::optional<std::int64_t> expected_revision) = 0;
    [[nodiscard]] virtual Result<LibraryStackMutation>
    set_stack_pick(std::string_view stack_id, std::string_view pick_asset_id,
                   std::optional<std::int64_t> expected_revision) = 0;
    [[nodiscard]] virtual Result<std::optional<LibraryStackRecord>>
    find_library_stack(std::string_view stack_id) const = 0;
    [[nodiscard]] virtual Result<std::vector<std::string>>
    list_version_asset_ids(std::string_view asset_id) const = 0;
    [[nodiscard]] virtual Result<std::optional<FolderRecord>>
    find_folder_by_id(std::string_view folder_id) const = 0;
    [[nodiscard]] virtual Result<std::vector<AssetRecord>>
    list_folder_assets(std::string_view folder_id) const = 0;
    // Revalidates the stable folder owner and exact asset URI set, then changes
    // the folder path, asset paths, recovery generations, and revision in one
    // transaction. Cancellation or failure before commit preserves every path.
    [[nodiscard]] virtual Result<void>
    commit_folder_relink(const FolderRelinkCommit &relink,
                         const CancellationToken &cancellation) = 0;
    [[nodiscard]] virtual Result<CatalogBackupPolicy> backup_policy() const = 0;
    [[nodiscard]] virtual Result<void> save_backup_policy(const CatalogBackupPolicy &policy) = 0;
    [[nodiscard]] virtual Result<std::optional<AssetRecord>>
    find_asset_by_id(std::string_view asset_id) const = 0;
    [[nodiscard]] virtual Result<std::optional<AssetRecord>>
    find_asset_by_uri(std::string_view normalized_uri) const = 0;
    // One transaction: asset row, optional capture row, revision bump.
    // Failure leaves no newly visible asset.
    [[nodiscard]] virtual Result<void> commit_imported_asset(const AssetRecord &asset) = 0;
    // One transaction: refresh the existing asset/capture rows and bump revision.
    // Failure preserves the previous asset, capture values, and revision.
    [[nodiscard]] virtual Result<void> commit_refreshed_asset(const AssetRecord &asset) = 0;
    [[nodiscard]] virtual Result<void> update_asset(const AssetRecord &asset) = 0;
    [[nodiscard]] virtual Result<void> update_review(std::string_view asset_id,
                                                     const ReviewState &review) = 0;
    // One transaction: delete the asset cascade and bump catalog revision.
    // Failure leaves the asset and prior revision visible.
    [[nodiscard]] virtual Result<void> remove_asset(std::string_view asset_id) = 0;
    [[nodiscard]] virtual Result<std::optional<std::string>>
    load_recipe_json(std::string_view asset_id) const = 0;
    [[nodiscard]] virtual Result<void> save_recipe_json(std::string_view asset_id,
                                                        std::int64_t recipe_schema_version,
                                                        std::string_view recipe_json) = 0;
    [[nodiscard]] virtual Result<void> clear_recipe(std::string_view asset_id) = 0;
    // Publishes the current recipe, optional newer-history deletion, the automatic history
    // entry, and the catalog revision as one transaction. A missing recipe_json clears the
    // current recipe while history_json keeps the explicit baseline state. discard_history_after_seq
    // deletes this asset's history rows with seq greater than that value before the optional
    // append. coalesce_history_id replaces that ordinary row only while it remains the latest
    // row for this asset; otherwise the new state appends without overwriting intervening work.
    // The repository must leave the previous recipe and history visible when any part of the
    // transaction fails.
    [[nodiscard]] virtual Result<RecipeCommitResult>
    commit_recipe(std::string_view asset_id, std::int64_t recipe_schema_version,
                  std::optional<std::string_view> recipe_json, std::string_view history_json,
                  RecipeHistoryWrite history_write,
                  std::optional<std::int64_t> discard_history_after_seq,
                  std::optional<std::int64_t> coalesce_history_id) = 0;
    [[nodiscard]] virtual Result<void> replace_asset_tags(std::string_view asset_id,
                                                          const std::vector<std::string> &tags) = 0;
    [[nodiscard]] virtual Result<std::vector<KeywordRecord>> list_keywords() const = 0;
    [[nodiscard]] virtual Result<std::optional<KeywordRecord>>
    find_keyword_by_id(std::string_view keyword_id) const = 0;
    [[nodiscard]] virtual Result<std::optional<KeywordRecord>>
    find_keyword_by_path(std::string_view path) const = 0;
    [[nodiscard]] virtual Result<KeywordMutation>
    create_keyword(std::string_view name, std::optional<std::string_view> parent_id,
                   std::optional<std::int64_t> expected_revision) = 0;
    [[nodiscard]] virtual Result<KeywordMutation>
    rename_keyword(std::string_view keyword_id, std::string_view name,
                   std::optional<std::int64_t> expected_revision) = 0;
    [[nodiscard]] virtual Result<KeywordMutation>
    move_keyword(std::string_view keyword_id, std::optional<std::string_view> parent_id,
                 std::optional<std::int64_t> expected_revision) = 0;
    [[nodiscard]] virtual Result<std::int64_t>
    delete_keyword(std::string_view keyword_id, bool recursive,
                   std::optional<std::int64_t> expected_revision) = 0;
    [[nodiscard]] virtual Result<KeywordMembershipMutation>
    replace_assets_tags(const std::vector<std::string> &asset_ids,
                        const std::vector<std::string> &tag_paths,
                        std::optional<std::int64_t> expected_revision) = 0;
    [[nodiscard]] virtual Result<void>
    upsert_writable_metadata(std::string_view asset_id, const WritableMetadata &metadata) = 0;
    [[nodiscard]] virtual Result<std::vector<RecipeHistoryEntry>>
    list_recipe_history(std::string_view asset_id) const = 0;
    [[nodiscard]] virtual Result<std::optional<RecipeHistoryEntry>>
    find_recipe_history(std::int64_t history_id) const = 0;
    [[nodiscard]] virtual Result<RecipeHistoryEntry>
    append_recipe_history(std::string_view asset_id, std::string_view kind,
                          std::optional<std::string_view> label, std::string_view recipe_json) = 0;
    [[nodiscard]] virtual Result<void> update_recipe_history_label(std::int64_t history_id,
                                                                   std::string_view label) = 0;
    [[nodiscard]] virtual Result<std::optional<PreviewRecord>>
    find_preview(std::string_view asset_id) const = 0;
    [[nodiscard]] virtual Result<std::vector<PreviewRecord>> list_previews() const = 0;
    [[nodiscard]] virtual Result<std::vector<PreviewRecord>>
    list_previews_for_assets(const std::vector<std::string> &asset_ids) const = 0;
    [[nodiscard]] virtual Result<void> upsert_preview(const PreviewRecord &preview) = 0;
    [[nodiscard]] virtual Result<AssetRecoveryState>
    recovery_state(std::string_view asset_id) const = 0;
    [[nodiscard]] virtual Result<std::vector<AssetRecoveryState>> list_pending_recovery() const = 0;
    [[nodiscard]] virtual Result<std::vector<AssetRecoveryState>> list_recovery_states() const = 0;
    [[nodiscard]] virtual Result<AssetRecoverySnapshot>
    load_recovery_snapshot(std::string_view asset_id) const = 0;
    // Acknowledges only the exact generation that was serialized. If another
    // catalog client committed newer state, the newer generation stays pending.
    [[nodiscard]] virtual Result<AssetRecoveryState>
    acknowledge_recovery(std::string_view asset_id, std::int64_t generation) = 0;
    [[nodiscard]] virtual Result<void> integrity_check() const = 0;
    [[nodiscard]] virtual Result<CatalogDatabaseArtifact>
    create_backup_database(std::string_view output_path,
                           const CancellationToken &cancellation) const = 0;
    [[nodiscard]] virtual Result<CatalogDatabaseArtifact>
    verify_backup_database(std::string_view backup_path, std::string_view expected_sha256,
                           const CancellationToken &cancellation) const = 0;
    [[nodiscard]] virtual Result<std::int64_t> bump_revision() = 0;
    virtual Result<void> close() = 0;
};

} // namespace ravo
