#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "ravo/domain/catalog_repository.h"

namespace ravo
{

class SqliteCatalogBackupVerifier final : public CatalogBackupDatabaseVerifier,
                                          public CatalogRestoreDatabaseVerifier
{
public:
    [[nodiscard]] Result<CatalogDatabaseArtifact>
    verify_backup_database(std::string_view backup_path, std::string_view expected_sha256,
                           const CancellationToken &cancellation) const override;
    [[nodiscard]] Result<CatalogSnapshot>
    verify_restored_catalog(std::string_view catalog_path, std::string_view expected_catalog_id,
                            const CancellationToken &cancellation) const override;
};

// ADR-0136 residual: rewrite support-rooted asset/folder URIs without bumping
// recovery generation. Fail-closed for unknown `{catalog}.ravo/` trees.
[[nodiscard]] Result<std::size_t>
sqlite_rewrite_support_rooted_uris(std::string_view catalog_path,
                                   std::string_view destination_support_root,
                                   const CancellationToken &cancellation);

namespace testing
{
class SqliteCatalogTestControl;
}

class SqliteCatalogRepository final : public CatalogRepository
{
public:
    static Result<std::unique_ptr<SqliteCatalogRepository>> create(std::string_view database_path);
    static Result<std::unique_ptr<SqliteCatalogRepository>> open(std::string_view database_path);

    SqliteCatalogRepository(const SqliteCatalogRepository &) = delete;
    SqliteCatalogRepository &operator=(const SqliteCatalogRepository &) = delete;
    ~SqliteCatalogRepository() override;

    [[nodiscard]] Result<CatalogSnapshot> snapshot() const override;
    [[nodiscard]] Result<std::vector<AssetRecord>> list_assets() const override;
    [[nodiscard]] Result<LibraryPage>
    list_assets_page(const LibraryPageRequest &request) const override;
    [[nodiscard]] Result<std::vector<FolderRecord>> list_folders() const override;
    [[nodiscard]] Result<std::vector<LibrarySetRecord>> list_library_sets() const override;
    [[nodiscard]] Result<std::optional<LibrarySetRecord>>
    find_library_set(std::string_view set_id) const override;
    [[nodiscard]] Result<LibrarySetMutation>
    create_library_set(LibrarySetKind kind, std::string_view name,
                       const std::optional<LibraryQuery> &query,
                       const std::vector<std::string> &asset_ids,
                       std::optional<std::int64_t> expected_revision) override;
    [[nodiscard]] Result<LibrarySetMutation>
    rename_library_set(std::string_view set_id, std::string_view name,
                       std::optional<std::int64_t> expected_revision) override;
    [[nodiscard]] Result<std::int64_t>
    delete_library_set(std::string_view set_id,
                       std::optional<std::int64_t> expected_revision) override;
    [[nodiscard]] Result<LibrarySetMutation>
    add_library_set_members(std::string_view set_id, const std::vector<std::string> &asset_ids,
                            std::optional<std::int64_t> expected_revision) override;
    [[nodiscard]] Result<LibrarySetMutation>
    remove_library_set_members(std::string_view set_id, const std::vector<std::string> &asset_ids,
                               std::optional<std::int64_t> expected_revision) override;
    [[nodiscard]] Result<AssetVersionMutation>
    create_asset_version(std::string_view source_asset_id,
                         std::optional<std::int64_t> expected_revision) override;
    [[nodiscard]] Result<LibraryStackMutation>
    stack_assets(const std::vector<std::string> &asset_ids, std::string_view pick_asset_id,
                 std::optional<std::int64_t> expected_revision) override;
    [[nodiscard]] Result<std::int64_t>
    unstack_assets(std::string_view stack_id,
                   std::optional<std::int64_t> expected_revision) override;
    [[nodiscard]] Result<LibraryStackMutation>
    set_stack_pick(std::string_view stack_id, std::string_view pick_asset_id,
                   std::optional<std::int64_t> expected_revision) override;
    [[nodiscard]] Result<std::optional<LibraryStackRecord>>
    find_library_stack(std::string_view stack_id) const override;
    [[nodiscard]] Result<std::vector<std::string>>
    list_version_asset_ids(std::string_view asset_id) const override;
    [[nodiscard]] Result<std::optional<FolderRecord>>
    find_folder_by_id(std::string_view folder_id) const override;
    [[nodiscard]] Result<std::vector<AssetRecord>>
    list_folder_assets(std::string_view folder_id) const override;
    [[nodiscard]] Result<void> commit_folder_relink(const FolderRelinkCommit &relink,
                                                    const CancellationToken &cancellation) override;
    [[nodiscard]] Result<CatalogBackupPolicy> backup_policy() const override;
    [[nodiscard]] Result<void> save_backup_policy(const CatalogBackupPolicy &policy) override;
    [[nodiscard]] Result<std::optional<AssetRecord>>
    find_asset_by_id(std::string_view asset_id) const override;
    [[nodiscard]] Result<std::optional<AssetRecord>>
    find_asset_by_uri(std::string_view normalized_uri) const override;
    [[nodiscard]] Result<void>
    commit_imported_asset(const AssetRecord &asset,
                          const std::optional<std::string> &sha256 = std::nullopt,
                          bool reject_duplicate_content = false) override;
    [[nodiscard]] Result<std::vector<ImportContentSource>>
    import_content_sources(std::uint64_t size_bytes,
                           std::string_view after_asset_id) const override;
    [[nodiscard]] Result<void> cache_import_content(const ImportContentSource &source,
                                                    std::string_view sha256) override;
    [[nodiscard]] Result<std::optional<std::string>>
    find_import_content(std::uint64_t size_bytes, std::string_view sha256) const override;
    [[nodiscard]] Result<void> commit_refreshed_asset(const AssetRecord &asset) override;
    [[nodiscard]] Result<void> update_asset(const AssetRecord &asset) override;
    [[nodiscard]] Result<void> update_review(std::string_view asset_id,
                                             const ReviewState &review) override;
    [[nodiscard]] Result<std::int64_t> commit_review(std::string_view asset_id,
                                                     const ReviewState &review) override;
    [[nodiscard]] Result<void> remove_asset(std::string_view asset_id) override;
    [[nodiscard]] Result<std::optional<std::string>>
    load_recipe_json(std::string_view asset_id) const override;
    [[nodiscard]] Result<void> save_recipe_json(std::string_view asset_id,
                                                std::int64_t recipe_schema_version,
                                                std::string_view recipe_json) override;
    [[nodiscard]] Result<void> clear_recipe(std::string_view asset_id) override;
    [[nodiscard]] Result<RecipeCommitResult>
    commit_recipe(std::string_view asset_id, std::int64_t recipe_schema_version,
                  std::optional<std::string_view> recipe_json, std::string_view history_json,
                  RecipeHistoryWrite history_write,
                  std::optional<std::int64_t> discard_history_after_seq,
                  std::optional<std::int64_t> coalesce_history_id) override;
    [[nodiscard]] Result<void> replace_asset_tags(std::string_view asset_id,
                                                  const std::vector<std::string> &tags) override;
    [[nodiscard]] Result<LibraryCaptureFacets> list_capture_facets() const override;
    [[nodiscard]] Result<LibraryLocationFacets> list_location_facets() const override;
    [[nodiscard]] Result<LibraryCaptureFacets>
    list_capture_facets(const LibraryQuery &scope) const override;
    [[nodiscard]] Result<LibraryLocationFacets>
    list_location_facets(const LibraryQuery &scope) const override;
    [[nodiscard]] Result<std::vector<KeywordRecord>> list_keywords() const override;
    [[nodiscard]] Result<std::optional<KeywordRecord>>
    find_keyword_by_id(std::string_view keyword_id) const override;
    [[nodiscard]] Result<std::optional<KeywordRecord>>
    find_keyword_by_path(std::string_view path) const override;
    [[nodiscard]] Result<KeywordMutation>
    create_keyword(std::string_view name, std::optional<std::string_view> parent_id,
                   std::optional<std::int64_t> expected_revision) override;
    [[nodiscard]] Result<KeywordMutation>
    rename_keyword(std::string_view keyword_id, std::string_view name,
                   std::optional<std::int64_t> expected_revision) override;
    [[nodiscard]] Result<KeywordMutation>
    move_keyword(std::string_view keyword_id, std::optional<std::string_view> parent_id,
                 std::optional<std::int64_t> expected_revision) override;
    [[nodiscard]] Result<std::int64_t>
    delete_keyword(std::string_view keyword_id, bool recursive,
                   std::optional<std::int64_t> expected_revision) override;
    [[nodiscard]] Result<KeywordMembershipMutation>
    replace_assets_tags(const std::vector<std::string> &asset_ids,
                        const std::vector<std::string> &tag_paths,
                        std::optional<std::int64_t> expected_revision) override;
    [[nodiscard]] Result<void> upsert_writable_metadata(std::string_view asset_id,
                                                        const WritableMetadata &metadata) override;
    [[nodiscard]] Result<WritableMetadataMutation>
    patch_assets_writable_metadata(const std::vector<std::string> &asset_ids,
                                   const WritableMetadataPatch &patch,
                                   std::optional<std::int64_t> expected_revision) override;
    [[nodiscard]] Result<std::vector<RecipeHistoryEntry>>
    list_recipe_history(std::string_view asset_id) const override;
    [[nodiscard]] Result<std::optional<RecipeHistoryEntry>>
    find_recipe_history(std::int64_t history_id) const override;
    [[nodiscard]] Result<RecipeHistoryEntry>
    append_recipe_history(std::string_view asset_id, std::string_view kind,
                          std::optional<std::string_view> label,
                          std::string_view recipe_json) override;
    [[nodiscard]] Result<void> update_recipe_history_label(std::int64_t history_id,
                                                           std::string_view label) override;
    [[nodiscard]] Result<std::optional<PreviewRecord>>
    find_preview(std::string_view asset_id) const override;
    [[nodiscard]] Result<std::vector<PreviewRecord>> list_previews() const override;
    [[nodiscard]] Result<std::vector<PreviewRecord>>
    list_previews_for_assets(const std::vector<std::string> &asset_ids) const override;
    [[nodiscard]] Result<void> upsert_preview(const PreviewRecord &preview) override;
    [[nodiscard]] Result<AssetRecoveryState>
    recovery_state(std::string_view asset_id) const override;
    [[nodiscard]] Result<std::vector<AssetRecoveryState>> list_pending_recovery() const override;
    [[nodiscard]] Result<std::vector<AssetRecoveryState>> list_recovery_states() const override;
    [[nodiscard]] Result<AssetRecoverySnapshot>
    load_recovery_snapshot(std::string_view asset_id) const override;
    [[nodiscard]] Result<AssetRecoveryState> acknowledge_recovery(std::string_view asset_id,
                                                                  std::int64_t generation) override;
    [[nodiscard]] Result<void> integrity_check() const override;
    [[nodiscard]] Result<CatalogDatabaseArtifact>
    create_backup_database(std::string_view output_path,
                           const CancellationToken &cancellation) const override;
    [[nodiscard]] Result<CatalogDatabaseArtifact>
    verify_backup_database(std::string_view backup_path, std::string_view expected_sha256,
                           const CancellationToken &cancellation) const override;
    [[nodiscard]] Result<std::int64_t> bump_revision() override;
    Result<void> close() override;

private:
    struct Impl;

    explicit SqliteCatalogRepository(std::unique_ptr<Impl> impl);
    static Result<std::unique_ptr<Impl>> open_database(std::string_view database_path, bool create);
    [[nodiscard]] Result<void> insert_asset(const AssetRecord &asset);
    [[nodiscard]] Result<void> upsert_capture_metadata(std::string_view asset_id,
                                                       const CaptureMetadata &capture);

    std::unique_ptr<Impl> impl_;

    friend class testing::SqliteCatalogTestControl;
};

} // namespace ravo
