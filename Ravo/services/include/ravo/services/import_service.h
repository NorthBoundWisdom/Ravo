#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <memory>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/domain/types.h"
#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"
#include "ravo/services/ingest_transport.h"
#include "ravo/engine/engine.h"

namespace ravo
{

class CatalogService;
class CatalogRepository;
class RasterDecoder;
class PreviewCache;
class PreviewService;
class RecoveryService;

class ImportService
{
public:
    ImportService(const ImportService &) = delete;
    ImportService &operator=(const ImportService &) = delete;
    ImportService(ImportService &&) = delete;
    ImportService &operator=(ImportService &&) = delete;

    [[nodiscard]] Result<ImportItemResult>
    import_one(std::string_view path, const CancellationToken &cancellation,
               ImportPreviewPolicy preview = ImportPreviewPolicy::kMinimal,
               bool defer_preview = false, bool skip_existing = false,
               std::string_view expected_sha256 = {});
    [[nodiscard]] Result<ImportCandidate>
    inspect_import_candidate(std::string_view path, std::string_view source_root,
                             const CancellationToken &cancellation) const;
    [[nodiscard]] Result<ImportScanResult> scan_import_candidates(
        const std::vector<std::string> &inputs, std::string_view source_root, bool recursive,
        const CancellationToken &cancellation,
        const std::function<void(std::size_t, std::size_t, const ImportCandidate &)> &progress = {},
        const std::function<void(const std::vector<std::string> &)> &enumerated = {});
    [[nodiscard]] Result<RasterBuffer>
    decode_import_candidate_thumbnail(std::string_view path,
                                      const CancellationToken &cancellation) const;
    [[nodiscard]] Result<RasterBuffer>
    decode_cull_fingerprint_raster(std::string_view path,
                                   const CancellationToken &cancellation) const;
    [[nodiscard]] Result<ImportBatchResult>
    execute_import(const ImportRequest &request,
                   const std::function<void(std::size_t, std::size_t, const ImportItemResult *)>
                       &progress = {});
    [[nodiscard]] Result<void> preflight_import(const ImportRequest &request);
    [[nodiscard]] Result<ImportDestinationPreview>
    preview_import_destinations(const ImportRequest &request);
    [[nodiscard]] Result<std::vector<std::string>>
    enumerate_import_inputs(const std::vector<std::string> &paths,
                            const CancellationToken &cancellation, bool recursive = true) const;
    [[nodiscard]] Result<std::vector<ImportItemResult>>
    import_inputs(const std::vector<std::string> &paths, const CancellationToken &cancellation,
                  const std::function<void(std::size_t, std::size_t, const ImportItemResult *)>
                      &progress = {});

private:
    friend class CatalogService;
    friend class IngestService;
    // The immovable composition owner supplies slots; reset observes close.
    ImportService(
        const std::unique_ptr<CatalogRepository> &repository,
        const std::unique_ptr<RasterDecoder> &raster, const EngineFacade *const &engine,
        const std::shared_ptr<PreviewCache> &cache, PreviewService &preview,
        RecoveryService &recovery, std::function<void()> &before_publication,
        const std::function<Result<void>(std::string_view, std::string_view)> &checkpoint) noexcept;
    [[nodiscard]] Result<CatalogSnapshot> library_snapshot() const;
    [[nodiscard]] Result<ImportCandidate>
    inspect_destination_candidate(std::string_view path, std::string_view source_root,
                                  const CancellationToken &cancellation);
    [[nodiscard]] Result<ImportScanResult> scan_import_candidates_impl(
        const std::vector<std::string> &inputs, std::string_view source_root, bool recursive,
        const CancellationToken &cancellation,
        const std::function<void(std::size_t, std::size_t, const ImportCandidate &)> &progress,
        const std::function<void(const std::vector<std::string> &)> &enumerated,
        bool require_stable_revision);
    [[nodiscard]] Result<ImportBatchResult> execute_import_impl(
        const ImportRequest &request,
        const std::function<void(std::size_t, std::size_t, const ImportItemResult *)> &progress,
        bool preflight_only, ImportDestinationPreview *destination_preview = nullptr);

    const std::unique_ptr<CatalogRepository> &repository_;
    const std::unique_ptr<RasterDecoder> &raster_;
    const EngineFacade *const &engine_;
    const std::shared_ptr<PreviewCache> &cache_;
    PreviewService &preview_service_;
    RecoveryService &recovery_service_;
    std::function<void()> &testing_before_import_publication_;
    const std::function<Result<void>(std::string_view, std::string_view)>
        &testing_import_checkpoint_;
    std::function<Result<void>()> ingest_source_liveness_;
    bool ingest_report_remaining_on_stop_ = false;
    // Serial service owner only. Metadata is provisional; import never uses this cache.
    std::map<std::string, ImportCandidate, std::less<>> destination_preview_candidates_;
};

} // namespace ravo
