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
#include "ravo/services/ingest_transport.h"

namespace ravo
{

class CatalogService;
class CatalogRepository;
class ImportService;

class IngestService
{
public:
    IngestService(const IngestService &) = delete;
    IngestService &operator=(const IngestService &) = delete;
    IngestService(IngestService &&) = delete;
    IngestService &operator=(IngestService &&) = delete;

    [[nodiscard]] Result<ImportBatchResult>
    execute_ingest(const IngestRequest &request,
                   const std::function<void(std::size_t, std::size_t, const ImportItemResult *)>
                       &progress = {});
    [[nodiscard]] Result<IngestBatchResult> execute_ingest_detailed(
        const IngestRequest &request,
        const std::function<void(std::size_t, std::size_t, const ImportItemResult *)> &progress =
            {});
    [[nodiscard]] Result<NativeIngestPlatformSupport> probe_ingest_native_support() const;

private:
    friend class CatalogService;
    IngestService(const std::unique_ptr<CatalogRepository> &repository,
                  ImportService &import_service) noexcept;
    const std::unique_ptr<CatalogRepository> &repository_;
    ImportService &import_service_;
};

} // namespace ravo
