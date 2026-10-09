#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
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
#include "ravo/services/ai_suggestion.h"
#include "ravo/services/xmp_interchange.h"
#include "ravo/services/external_editor.h"
#include "ravo/services/foreign_catalog.h"
#include "ravo/services/dng_smart_preview.h"
#include "ravo/services/offline_edit_proxy.h"
#include "ravo/services/cull_assistance.h"
#include "ravo/services/ingest_transport.h"
#include "ravo/services/photo_merge.h"
#include "ravo/services/develop_types.h"
#include "ravo/services/video.h"

namespace ravo
{

namespace testing
{
class CatalogServiceTestControl;
}

class LibraryService;
class AiService;
class ConversionService;
class CullService;
class ExportService;
class PhotoMergeService;
class XmpInterchangeService;
class ExternalEditorService;
class OfflineEditService;

class PreviewService;
class DevelopService;
class MetadataService;
class ImportService;
class IngestService;
class RecoveryService;

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
    // Each service and its Engine/repository/decoder stay on one owner thread.
    // Concurrent services for one catalog share a synchronized cache and the
    // same recovery-publication mutex; the service itself is not thread-safe.
    CatalogService(const EngineFacade &engine, std::unique_ptr<CatalogRepository> repository,
                   std::unique_ptr<RasterDecoder> raster, std::shared_ptr<PreviewCache> cache,
                   std::unique_ptr<RecoveryStore> recovery,
                   std::shared_ptr<std::mutex> recovery_publication_mutex = {},
                   std::unique_ptr<VideoDecoder> video = {});

    CatalogService(const CatalogService &) = delete;
    CatalogService &operator=(const CatalogService &) = delete;
    // Capabilities borrow resource slots; moving the owner would dangle them.
    CatalogService(CatalogService &&) = delete;
    CatalogService &operator=(CatalogService &&) = delete;
    ~CatalogService();

    // Use-case owners in the same target; this object composes their resources.
    [[nodiscard]] PreviewService &preview() noexcept;
    [[nodiscard]] const PreviewService &preview() const noexcept;
    [[nodiscard]] LibraryService &library() noexcept;
    [[nodiscard]] const LibraryService &library() const noexcept;
    [[nodiscard]] DevelopService &develop() noexcept;
    [[nodiscard]] const DevelopService &develop() const noexcept;
    [[nodiscard]] MetadataService &metadata() noexcept;
    [[nodiscard]] const MetadataService &metadata() const noexcept;
    [[nodiscard]] ImportService &import() noexcept;
    [[nodiscard]] const ImportService &import() const noexcept;
    [[nodiscard]] IngestService &ingest() noexcept;
    [[nodiscard]] const IngestService &ingest() const noexcept;
    [[nodiscard]] RecoveryService &recovery() noexcept;
    [[nodiscard]] const RecoveryService &recovery() const noexcept;

    [[nodiscard]] AiService &ai() noexcept;
    [[nodiscard]] const AiService &ai() const noexcept;
    [[nodiscard]] ConversionService &conversion() noexcept;
    [[nodiscard]] const ConversionService &conversion() const noexcept;
    [[nodiscard]] CullService &cull() noexcept;
    [[nodiscard]] const CullService &cull() const noexcept;
    [[nodiscard]] ExportService &exports() noexcept;
    [[nodiscard]] const ExportService &exports() const noexcept;
    [[nodiscard]] PhotoMergeService &merge() noexcept;
    [[nodiscard]] const PhotoMergeService &merge() const noexcept;
    [[nodiscard]] XmpInterchangeService &xmp() noexcept;
    [[nodiscard]] const XmpInterchangeService &xmp() const noexcept;
    [[nodiscard]] ExternalEditorService &external_editor() noexcept;
    [[nodiscard]] const ExternalEditorService &external_editor() const noexcept;
    [[nodiscard]] OfflineEditService &offline() noexcept;
    [[nodiscard]] const OfflineEditService &offline() const noexcept;

    Result<void> close();
    [[nodiscard]] Result<VideoInfo> video_info(std::string_view asset_id,
                                               const CancellationToken &cancellation = {}) const;
    [[nodiscard]] Result<VideoPreview>
    video_frame(std::string_view asset_id, std::int64_t time_us, std::uint32_t max_edge,
                const CancellationToken &cancellation = {}) const;

private:
    const EngineFacade *engine_ = nullptr;
    std::unique_ptr<CatalogRepository> repository_;
    std::unique_ptr<RasterDecoder> raster_;
    std::unique_ptr<VideoDecoder> video_;
    std::shared_ptr<PreviewCache> cache_;
    std::unique_ptr<RecoveryStore> recovery_;
    std::shared_ptr<std::mutex> recovery_publication_mutex_;
    std::function<void()> testing_before_import_publication_;
    std::function<Result<void>(std::string_view)> testing_merge_checkpoint_;
    std::function<void()> testing_before_preview_cache_publication_;
    std::function<Result<void>(std::string_view, std::string_view)> testing_import_checkpoint_;
    std::function<Result<void>(std::string_view, std::string_view)> testing_backup_checkpoint_;
    // Optional publish-boundary inject for offline-edit proxy staging replace.
    std::function<Result<void>(std::string_view final_root, std::string_view staging_root)>
        testing_before_offline_proxy_publish_;
    // Dependencies outlive their capabilities; dependents are destroyed first.
    std::unique_ptr<RecoveryService> recovery_capability_;
    std::unique_ptr<PreviewService> preview_capability_;
    std::unique_ptr<LibraryService> library_capability_;
    std::unique_ptr<DevelopService> develop_capability_;
    std::unique_ptr<MetadataService> metadata_capability_;
    std::unique_ptr<ImportService> import_capability_;
    std::unique_ptr<IngestService> ingest_capability_;

    std::unique_ptr<AiService> ai_capability_;
    std::unique_ptr<ConversionService> conversion_capability_;
    std::unique_ptr<CullService> cull_capability_;
    std::unique_ptr<ExportService> exports_capability_;
    std::unique_ptr<PhotoMergeService> merge_capability_;
    std::unique_ptr<XmpInterchangeService> xmp_capability_;
    std::unique_ptr<ExternalEditorService> external_editor_capability_;
    std::unique_ptr<OfflineEditService> offline_capability_;

    friend class testing::CatalogServiceTestControl;
};

} // namespace ravo

#include "ravo/services/library_service.h"
#include "ravo/services/develop_service.h"
#include "ravo/services/metadata_service.h"
#include "ravo/services/import_service.h"
#include "ravo/services/ingest_service.h"
#include "ravo/services/recovery_service.h"

#include "ravo/services/preview_service.h"

#include "ravo/services/ai_service.h"
#include "ravo/services/conversion_service.h"
#include "ravo/services/cull_service.h"
#include "ravo/services/exports_service.h"
#include "ravo/services/merge_service.h"
#include "ravo/services/xmp_service.h"
#include "ravo/services/external_editor_service.h"
#include "ravo/services/offline_service.h"
