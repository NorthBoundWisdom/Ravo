#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/domain/catalog_repository.h"
#include "ravo/domain/preview_cache.h"
#include "ravo/domain/raster_decoder.h"
#include "ravo/domain/recovery_store.h"
#include "ravo/domain/types.h"
#include "ravo/engine/engine.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"
#include "ravo/recipe/develop.h"
#include "ravo/recipe/recipe.h"
#include "ravo/services/ai_proposal.h"
#include "ravo/services/xmp_interchange.h"
#include "ravo/services/external_editor.h"
#include "ravo/services/foreign_catalog.h"
#include "ravo/services/ingest_transport.h"

namespace ravo
{

namespace testing
{
class CatalogServiceTestControl;
}

struct RecipeSaveOptions
{
    RecipeHistoryWrite history_write = RecipeHistoryWrite::kAppendIfNew;
    std::optional<std::int64_t> discard_history_after_seq;
    std::optional<std::int64_t> coalesce_history_id;
    // Studio's serial Develop owner may defer filesystem publication until the
    // new preview has been queued for the UI. The same worker then drains the
    // durable generation; close, reopen, explicit sync, and backup remain
    // recovery paths if that publication fails.
    bool defer_recovery_publication = false;
};

struct RecipeSaveResult
{
    AssetRecord asset;
    std::int64_t revision = 0;
    std::optional<std::int64_t> history_id;
};

struct DevelopApplyRequest
{
    DevelopParams source;
    std::vector<std::string> fields;
    std::vector<std::string> asset_ids;
    std::optional<std::int64_t> expected_revision;
    CancellationToken cancellation;
};

enum class DevelopApplyItemStatus : std::uint8_t
{
    kApplied = 0,
    kFailed = 1,
    kSkipped = 2,
};

struct DevelopApplyItemResult
{
    std::string asset_id;
    DevelopApplyItemStatus status = DevelopApplyItemStatus::kFailed;
    std::optional<std::int64_t> history_id;
    std::optional<TaskError> error;
};

struct DevelopApplyResult
{
    std::size_t applied = 0;
    std::size_t failed = 0;
    std::size_t skipped = 0;
    std::int64_t revision = 0;
    std::vector<DevelopApplyItemResult> items;
};

using DevelopApplyProgressCallback =
    std::function<void(std::size_t completed, std::size_t total, const DevelopApplyItemResult *)>;

[[nodiscard]] inline std::string_view
develop_apply_item_status_name(const DevelopApplyItemStatus status) noexcept
{
    switch (status)
    {
    case DevelopApplyItemStatus::kApplied:
        return "applied";
    case DevelopApplyItemStatus::kFailed:
        return "failed";
    case DevelopApplyItemStatus::kSkipped:
        return "skipped";
    }
    return "failed";
}

// Verifies a self-contained backup without opening or mutating a live catalog.
[[nodiscard]] Result<CatalogBackupVerification>
verify_catalog_backup(const CatalogBackupDatabaseVerifier &database_verifier,
                      const RecoveryStore &recovery_verifier, std::string_view backup_directory,
                      const CancellationToken &cancellation = {});

using CatalogRestoreProgressCallback = std::function<void(const CatalogRestoreProgress &)>;

// Restores a self-contained verified backup to an absent catalog path. The
// support root publishes first; the catalog file is the final visibility point.
// After that point errors carry restore_published=true and no path is removed.
[[nodiscard]] Result<CatalogRestoreResult>
restore_catalog_backup(const CatalogBackupDatabaseVerifier &backup_database_verifier,
                       const CatalogRestoreDatabaseVerifier &restored_database_verifier,
                       const RecoveryStore &recovery_verifier, const CatalogRestoreRequest &request,
                       const CatalogRestoreProgressCallback &progress = {});

class CatalogService
{
public:
    CatalogService(const EngineFacade &engine, std::unique_ptr<CatalogRepository> repository,
                   std::unique_ptr<RasterDecoder> raster, std::unique_ptr<PreviewCache> cache,
                   std::unique_ptr<RecoveryStore> recovery);

    CatalogService(const CatalogService &) = delete;
    CatalogService &operator=(const CatalogService &) = delete;
    CatalogService(CatalogService &&) noexcept = default;
    CatalogService &operator=(CatalogService &&) noexcept = default;
    ~CatalogService();

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
    [[nodiscard]] Result<AssetRecoveryState> recovery_state(std::string_view asset_id) const;
    [[nodiscard]] Result<std::vector<AssetRecoveryState>> pending_recovery() const;
    // With an asset ID, this also verifies an already-synchronized sidecar.
    // Without one, it drains only the durable pending set so catalog open stays
    // bounded by unfinished work rather than the full library size.
    [[nodiscard]] Result<RecoverySyncResult>
    sync_recovery(std::optional<std::string_view> asset_id,
                  const CancellationToken &cancellation = {});
    [[nodiscard]] Result<CatalogBackupArtifact>
    create_backup(std::string_view destination, const CancellationToken &cancellation = {});
    [[nodiscard]] Result<CatalogBackupVerification>
    verify_backup(std::string_view backup_directory,
                  const CancellationToken &cancellation = {}) const;
    [[nodiscard]] Result<CatalogBackupPolicy> backup_policy() const;
    [[nodiscard]] Result<CatalogBackupPolicy> set_backup_policy(CatalogBackupPolicy policy,
                                                                std::int64_t now_unix_ms);
    [[nodiscard]] Result<CatalogBackupScheduleResult>
    run_scheduled_backup(std::int64_t now_unix_ms, const CancellationToken &cancellation = {},
                         bool force = false);
    [[nodiscard]] Result<AssetRecord> set_rating(std::string_view asset_id, int rating);
    [[nodiscard]] Result<AssetRecord> set_color_label(std::string_view asset_id, ColorLabel label);
    [[nodiscard]] Result<AssetRecord> set_rejected(std::string_view asset_id, bool rejected);
    [[nodiscard]] Result<void> remove_from_catalog(std::string_view asset_id);
    [[nodiscard]] Result<void> remove_original_and_catalog(std::string_view asset_id);
    [[nodiscard]] Result<AssetRecord>
    refresh_capture_metadata(std::string_view asset_id, const CancellationToken &cancellation);
    [[nodiscard]] Result<bool> asset_has_edits(std::string_view asset_id) const;
    [[nodiscard]] Result<Recipe> load_recipe(std::string_view asset_id) const;
    [[nodiscard]] Result<Recipe> load_baseline_recipe(std::string_view asset_id) const;
    [[nodiscard]] Result<AssetRecord> save_recipe(std::string_view asset_id, const Recipe &recipe,
                                                  RecipeSaveOptions options = {});
    [[nodiscard]] Result<AssetRecord> save_develop(std::string_view asset_id,
                                                   const DevelopParams &params,
                                                   RecipeSaveOptions options = {});
    [[nodiscard]] Result<RecipeSaveResult>
    save_develop_with_history(std::string_view asset_id, const DevelopParams &params,
                              RecipeSaveOptions options = {});
    [[nodiscard]] Result<DevelopApplyResult>
    apply_develop_selection(const DevelopApplyRequest &request,
                            const DevelopApplyProgressCallback &progress = {});
    [[nodiscard]] Result<std::array<double, 4>>
    sample_white_balance(std::string_view asset_id, const WhiteBalancePickRequest &request,
                         const CancellationToken &cancellation);
    [[nodiscard]] Result<AssetRecord> reset_recipe(std::string_view asset_id);
    [[nodiscard]] Result<AssetRecord> set_tags(std::string_view asset_id,
                                               const std::vector<std::string> &tags);
    [[nodiscard]] Result<KeywordMembershipMutation>
    set_tags_selection(const std::vector<std::string> &asset_ids,
                       const std::vector<std::string> &tags,
                       std::optional<std::int64_t> expected_revision = std::nullopt);
    [[nodiscard]] Result<LibraryCaptureFacets> list_capture_facets() const;
    [[nodiscard]] Result<LibraryLocationFacets> list_location_facets() const;
    // Facet counts limited to the assets selected by `scope`.
    [[nodiscard]] Result<LibraryCaptureFacets> list_capture_facets(const LibraryQuery &scope) const;
    [[nodiscard]] Result<LibraryLocationFacets>
    list_location_facets(const LibraryQuery &scope) const;
    [[nodiscard]] Result<std::vector<KeywordRecord>> list_keywords() const;
    [[nodiscard]] Result<KeywordMutation>
    create_keyword(std::string_view name, std::optional<std::string_view> parent_id = std::nullopt,
                   std::optional<std::int64_t> expected_revision = std::nullopt);
    [[nodiscard]] Result<KeywordMutation>
    rename_keyword(std::string_view keyword_id, std::string_view name,
                   std::optional<std::int64_t> expected_revision = std::nullopt);
    [[nodiscard]] Result<KeywordMutation>
    move_keyword(std::string_view keyword_id,
                 std::optional<std::string_view> parent_id = std::nullopt,
                 std::optional<std::int64_t> expected_revision = std::nullopt);
    [[nodiscard]] Result<std::int64_t>
    delete_keyword(std::string_view keyword_id, bool recursive = false,
                   std::optional<std::int64_t> expected_revision = std::nullopt);
    [[nodiscard]] Result<AssetRecord> set_writable_metadata(std::string_view asset_id,
                                                            const WritableMetadata &metadata);
    [[nodiscard]] Result<WritableMetadataMutation>
    set_writable_metadata_selection(const std::vector<std::string> &asset_ids,
                                    const WritableMetadataPatch &patch,
                                    std::optional<std::int64_t> expected_revision = std::nullopt);
    [[nodiscard]] Result<std::vector<RecipeHistoryEntry>>
    list_recipe_history(std::string_view asset_id) const;
    [[nodiscard]] Result<AssetRecord> create_recipe_snapshot(std::string_view asset_id,
                                                             std::string_view label);
    [[nodiscard]] Result<AssetRecord> rename_recipe_snapshot(std::string_view asset_id,
                                                             std::int64_t history_id,
                                                             std::string_view label);
    [[nodiscard]] Result<AssetRecord> restore_recipe_history(std::string_view asset_id,
                                                             std::int64_t history_id);
    [[nodiscard]] Result<ImportItemResult>
    import_one(std::string_view path, const CancellationToken &cancellation,
               ImportPreviewPolicy preview = ImportPreviewPolicy::kMinimal,
               bool defer_preview = false);
    [[nodiscard]] Result<ImportCandidate>
    inspect_import_candidate(std::string_view path, std::string_view source_root,
                             const CancellationToken &cancellation) const;
    [[nodiscard]] Result<RasterBuffer>
    decode_import_candidate_thumbnail(std::string_view path,
                                      const CancellationToken &cancellation) const;
    [[nodiscard]] Result<ImportBatchResult>
    execute_import(const ImportRequest &request,
                   const std::function<void(std::size_t, std::size_t, const ImportItemResult *)>
                       &progress = {});
    // ADR-0125: filesystem-card (DCIM/mount) ingest with disconnect/cancel
    // reporting. Rejects Move; feeds the existing Add/Copy planner.
    [[nodiscard]] Result<ImportBatchResult>
    execute_ingest(const IngestRequest &request,
                   const std::function<void(std::size_t, std::size_t, const ImportItemResult *)>
                       &progress = {});
    [[nodiscard]] Result<PreviewResult> build_import_preview(std::string_view asset_id,
                                                             ImportPreviewPolicy policy,
                                                             const CancellationToken &cancellation);
    [[nodiscard]] Result<std::vector<std::string>>
    enumerate_import_inputs(const std::vector<std::string> &paths,
                            const CancellationToken &cancellation, bool recursive = true) const;
    [[nodiscard]] Result<std::vector<ImportItemResult>>
    import_inputs(const std::vector<std::string> &paths, const CancellationToken &cancellation,
                  const std::function<void(std::size_t, std::size_t, const ImportItemResult *)>
                      &progress = {});
    [[nodiscard]] Result<PreviewResult>
    request_preview(const PreviewRequest &request,
                    const std::optional<DevelopParams> &live_develop = {});
    [[nodiscard]] Result<PreviewRebuildResult> rebuild_previews(
        const std::vector<std::string> &asset_ids, const CancellationToken &cancellation,
        const std::function<void(std::size_t, std::size_t, const PreviewRebuildItemResult *)>
            &progress = {});
    [[nodiscard]] Result<ExportResult> export_asset(const ExportRequest &request);
    [[nodiscard]] Result<std::vector<ExportResult>> export_assets(
        const ExportBatchRequest &request,
        const std::function<void(std::size_t, std::size_t, const ExportResult *)> &progress = {});
    [[nodiscard]] Result<ExportJob> create_export_job(const ExportBatchRequest &request,
                                                      std::string job_id);
    [[nodiscard]] Result<ExportJob> run_export_job(
        ExportJob job,
        const std::function<void(std::size_t, std::size_t, const ExportResult *)> &progress = {});
    [[nodiscard]] Result<ExportJob> resume_export_job(
        ExportJob job,
        const std::function<void(std::size_t, std::size_t, const ExportResult *)> &progress = {});

    [[nodiscard]] Result<XmpInterchangeStatus>
    xmp_interchange_status(std::string_view asset_id,
                           std::optional<std::string_view> sidecar_path = std::nullopt) const;
    [[nodiscard]] Result<XmpInterchangeImportResult>
    xmp_interchange_import(std::string_view asset_id, XmpInterchangeResolve resolve,
                           std::optional<std::string_view> sidecar_path = std::nullopt);
    [[nodiscard]] Result<XmpInterchangeExportResult>
    xmp_interchange_export(std::string_view asset_id, XmpInterchangeResolve resolve,
                           std::optional<std::string_view> sidecar_path = std::nullopt);

    // ADR-0122: register external-editor output as a new derived asset with
    // provenance. Never mutates the source original; never launches an editor.
    [[nodiscard]] Result<ExternalEditorRegisterResult>
    register_external_editor_output(const ExternalEditorRegisterRequest &request);
    [[nodiscard]] Result<ExternalEditorProvenance>
    external_editor_provenance(std::string_view derived_asset_id) const;

    // ADR-0131: read-only foreign catalog conversion into this NEW/empty Ravo
    // catalog. Never opens or migrates the source in place; originals stay
    // byte-identical; unsupported schema/version fails closed.
    [[nodiscard]] Result<ForeignCatalogConversionReport>
    convert_foreign_catalog(const ForeignCatalogConversionRequest &request);

    // AI-01: reviewable global edit proposals (ADR-0121). Session-scoped until
    // applied; reject/cancel never mutate recipe/history.
    [[nodiscard]] Result<AiProposal> create_ai_proposal(const AiProposalCreateRequest &request);
    [[nodiscard]] Result<std::vector<AiProposal>>
    create_shoot_consistency_proposals(const AiShootConsistencyRequest &request);
    [[nodiscard]] Result<AiProposal> get_ai_proposal(std::string_view proposal_id) const;
    [[nodiscard]] Result<std::vector<AiProposal>>
    list_ai_proposals(std::optional<std::string_view> asset_id = std::nullopt) const;
    [[nodiscard]] Result<AiProposalApplyResult>
    apply_ai_proposal(std::string_view proposal_id,
                      std::optional<std::int64_t> expected_catalog_revision = std::nullopt);
    [[nodiscard]] Result<AiProposal> reject_ai_proposal(std::string_view proposal_id);
    [[nodiscard]] Result<AiProposal> cancel_ai_proposal(std::string_view proposal_id);

    Result<void> close();

private:
    [[nodiscard]] Result<RecoveryArtifact>
    synchronize_recovery_asset(std::string_view asset_id, const CancellationToken &cancellation);
    [[nodiscard]] Result<void>
    synchronize_committed_change(std::string_view asset_id,
                                 const CancellationToken &cancellation = {});
    [[nodiscard]] Result<RecipeSaveResult> save_recipe_with_history(std::string_view asset_id,
                                                                    const Recipe &recipe,
                                                                    RecipeSaveOptions options);

    enum class PreviewLane
    {
        kForegroundDevelop,
        kBackgroundBrowse,
    };

    struct DecodedPreviewSource
    {
        std::string asset_id;
        std::string fingerprint;
        std::uint32_t max_edge = 0;
        RasterBuffer raster;
    };

    struct CachedRawFrame
    {
        std::string asset_id;
        std::string fingerprint;
        std::string path;
        DecodedRaw raw;
    };

    struct CachedLinearWorking
    {
        std::string asset_id;
        std::string fingerprint;
        std::uint32_t max_edge = 0;
        std::string preprocess_key;
        LinearWorkingBuffer buffer;
        InteractivePreviewRenderCache interactive_render_cache;
    };

    struct CachedRoiLinearWorking
    {
        std::string asset_id;
        std::string fingerprint;
        std::string preprocess_key;
        std::uint32_t origin_x = 0;
        std::uint32_t origin_y = 0;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        LinearWorkingBuffer buffer;
        InteractivePreviewRenderCache interactive_render_cache;
        std::uint64_t generation = 0;
    };

    [[nodiscard]] Result<PreviewResult>
    generate_preview(const AssetRecord &asset, const PreviewRequest &request,
                     const std::optional<DevelopParams> &live_develop);
    [[nodiscard]] Result<PreviewResult> generate_roi_preview(const AssetRecord &asset,
                                                             const PreviewRequest &request,
                                                             const Recipe &recipe,
                                                             std::string_view path);
    [[nodiscard]] Result<PreviewResult>
    persist_embedded_browse_preview(const AssetRecord &asset, const EmbeddedPreview &embedded,
                                    std::uint32_t max_edge, const CancellationToken &cancellation);
    [[nodiscard]] Result<PreviewResult>
    persist_companion_jpeg_browse_preview(const AssetRecord &asset, std::string_view jpeg_path,
                                          std::uint32_t max_edge,
                                          const CancellationToken &cancellation);
    [[nodiscard]] Result<RasterBuffer>
    decode_preview_source(const AssetRecord &asset, std::string_view path, std::uint32_t max_edge,
                          const CancellationToken &cancellation, PreviewLane lane);
    [[nodiscard]] Result<const DecodedRaw *> cached_raw_frame(const AssetRecord &asset,
                                                              std::string_view path,
                                                              const CancellationToken &cancellation,
                                                              PreviewLane lane);
    [[nodiscard]] Result<CachedLinearWorking *>
    cached_linear_working(const AssetRecord &asset, std::string_view path, const Recipe &recipe,
                          std::uint32_t width, std::uint32_t height, std::uint32_t max_edge,
                          const CancellationToken &cancellation, PreviewLane lane);
    [[nodiscard]] Result<CachedRoiLinearWorking *>
    cached_roi_linear_working(const AssetRecord &asset, std::string_view path, const Recipe &recipe,
                              std::uint32_t origin_x, std::uint32_t origin_y, std::uint32_t width,
                              std::uint32_t height, const CancellationToken &cancellation);
    [[nodiscard]] Result<RenderedExportImage>
    render_for_export(const AssetRecord &asset, std::string_view path, const Recipe &recipe,
                      const ExportOptions &options, const CancellationToken &cancellation,
                      RenderSampleKind sample_kind);

    const EngineFacade *engine_ = nullptr;
    std::unique_ptr<CatalogRepository> repository_;
    std::unique_ptr<RasterDecoder> raster_;
    std::unique_ptr<PreviewCache> cache_;
    std::unique_ptr<RecoveryStore> recovery_;
    // Foreground Develop and background Gallery work have independent bounded
    // decode/working ownership. A thumbnail must never evict the selected
    // photo's interactive or settled scene-linear buffers.
    std::optional<DecodedPreviewSource> decoded_preview_source_;
    std::optional<CachedRawFrame> decoded_raw_;
    // Preview interaction alternates between the live frame and the
    // 1600px settled frame. Keep one bounded slot for each size class so the
    // live request cannot evict the already-prepared settled working buffer.
    // Interactive-class requests may box-filter the settled slot instead of a
    // second CFA demosaic; settled and larger sizes stay native. Viewport 1:1
    // owns one additional CFA-window slot so RGB sliders do not remosaic.
    std::array<std::optional<CachedLinearWorking>, 2> linear_working_;
    std::optional<CachedRoiLinearWorking> roi_linear_working_;
    std::optional<DecodedPreviewSource> browse_decoded_preview_source_;
    std::optional<CachedRawFrame> browse_decoded_raw_;
    std::optional<CachedLinearWorking> browse_linear_working_;
    std::function<void()> testing_before_import_publication_;
    std::function<void()> testing_before_preview_cache_publication_;
    std::function<Result<void>(std::string_view, std::string_view)> testing_import_checkpoint_;
    std::function<Result<void>(std::string_view, std::string_view)> testing_backup_checkpoint_;
    // ADR-0125 ingest transport liveness (filesystem-card disconnect).
    std::function<Result<void>()> ingest_source_liveness_;
    bool ingest_report_remaining_on_stop_ = false;
    mutable std::map<std::string, AiProposal, std::less<>> ai_proposals_;
    mutable bool ai_proposals_loaded_ = false;

    [[nodiscard]] Result<std::filesystem::path> ai_proposals_directory() const;
    [[nodiscard]] Result<void> ensure_ai_proposals_loaded() const;
    [[nodiscard]] Result<void> persist_ai_proposal(const AiProposal &proposal);

    friend class testing::CatalogServiceTestControl;
};

} // namespace ravo
