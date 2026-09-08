#include "ravo/services/ingest_service.h"

#include "ravo/services/catalog_service.h"

namespace ravo
{

IngestService::IngestService(CatalogService &catalog) noexcept
    : catalog_(&catalog)
{
}

Result<ImportBatchResult> IngestService::execute_ingest(
    const IngestRequest &request,
    const std::function<void(std::size_t, std::size_t, const ImportItemResult *)> &progress)
{
    return catalog_->execute_ingest(request, progress);
}

Result<IngestBatchResult> IngestService::execute_ingest_detailed(
    const IngestRequest &request,
    const std::function<void(std::size_t, std::size_t, const ImportItemResult *)> &progress)
{
    return catalog_->execute_ingest_detailed(request, progress);
}

Result<NativeIngestPlatformSupport> IngestService::probe_ingest_native_support() const
{
    return catalog_->probe_ingest_native_support();
}

} // namespace ravo
