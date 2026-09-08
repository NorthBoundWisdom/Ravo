#include "ravo/services/import_service.h"

#include "ravo/services/catalog_service.h"

namespace ravo
{

ImportService::ImportService(CatalogService &catalog) noexcept
    : catalog_(&catalog)
{
}

Result<ImportItemResult>
ImportService::import_one(const std::string_view path, const CancellationToken &cancellation,
                          const ImportPreviewPolicy preview, const bool defer_preview,
                          const bool skip_existing, const std::string_view expected_sha256)
{
    return catalog_->import_one(path, cancellation, preview, defer_preview, skip_existing,
                                expected_sha256);
}

Result<ImportScanResult> ImportService::scan_import_candidates(
    const std::vector<std::string> &inputs, const std::string_view source_root,
    const bool recursive, const CancellationToken &cancellation,
    const std::function<void(std::size_t, std::size_t, const ImportCandidate &)> &progress,
    const std::function<void(const std::vector<std::string> &)> &enumerated)
{
    return catalog_->scan_import_candidates(inputs, source_root, recursive, cancellation, progress,
                                            enumerated);
}

Result<ImportBatchResult> ImportService::execute_import(
    const ImportRequest &request,
    const std::function<void(std::size_t, std::size_t, const ImportItemResult *)> &progress)
{
    return catalog_->execute_import(request, progress);
}

Result<void> ImportService::preflight_import(const ImportRequest &request)
{
    return catalog_->preflight_import(request);
}

Result<ImportDestinationPreview>
ImportService::preview_import_destinations(const ImportRequest &request)
{
    return catalog_->preview_import_destinations(request);
}

Result<std::vector<std::string>>
ImportService::enumerate_import_inputs(const std::vector<std::string> &paths,
                                       const CancellationToken &cancellation,
                                       const bool recursive) const
{
    return catalog_->enumerate_import_inputs(paths, cancellation, recursive);
}

} // namespace ravo
