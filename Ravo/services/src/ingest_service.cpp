#include "ravo/services/ingest_service.h"

namespace ravo
{
IngestService::IngestService(const std::unique_ptr<CatalogRepository> &repository,
                             ImportService &import_service) noexcept
    : repository_(repository)
    , import_service_(import_service)
{
}
} // namespace ravo
