#pragma once

#include <cstdint>
#include <functional>
#include <optional>
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

class IngestService
{
public:
    explicit IngestService(CatalogService &catalog) noexcept;

    IngestService(const IngestService &) = delete;
    IngestService &operator=(const IngestService &) = delete;
    IngestService(IngestService &&) noexcept = default;
    IngestService &operator=(IngestService &&) noexcept = default;

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
    CatalogService *catalog_ = nullptr;
};

} // namespace ravo
