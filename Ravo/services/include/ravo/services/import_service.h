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

class ImportService
{
public:
    explicit ImportService(CatalogService &catalog) noexcept;

    ImportService(const ImportService &) = delete;
    ImportService &operator=(const ImportService &) = delete;
    ImportService(ImportService &&) noexcept = default;
    ImportService &operator=(ImportService &&) noexcept = default;

    [[nodiscard]] Result<ImportItemResult>
    import_one(std::string_view path, const CancellationToken &cancellation,
               ImportPreviewPolicy preview = ImportPreviewPolicy::kMinimal,
               bool defer_preview = false, bool skip_existing = false,
               std::string_view expected_sha256 = {});
    [[nodiscard]] Result<ImportScanResult> scan_import_candidates(
        const std::vector<std::string> &inputs, std::string_view source_root, bool recursive,
        const CancellationToken &cancellation,
        const std::function<void(std::size_t, std::size_t, const ImportCandidate &)> &progress = {},
        const std::function<void(const std::vector<std::string> &)> &enumerated = {});
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

private:
    CatalogService *catalog_ = nullptr;
};

} // namespace ravo
