#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ravo/domain/types.h"

namespace ravo
{
class CatalogService;
class CatalogRepository;
class RasterDecoder;
class EngineFacade;
class PreviewCache;
class RecoveryStore;
class PreviewService;

// One owner-thread use-case service. Borrowed slots/capabilities outlive this
// object; closed resource slots preserve the existing structured failures.
class ExportService
{
public:
    ExportService(const ExportService &) = delete;
    ExportService &operator=(const ExportService &) = delete;
    ExportService(ExportService &&) = delete;
    ExportService &operator=(ExportService &&) = delete;

    [[nodiscard]] Result<ExportResult> export_asset(const ExportRequest &request);
    [[nodiscard]] Result<void> check_companion_jpegs(const std::vector<std::string> &asset_ids,
                                                     const CancellationToken &cancellation);
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

private:
    friend class CatalogService;
    ExportService(const std::unique_ptr<CatalogRepository> &repository,
                  const std::unique_ptr<RasterDecoder> &raster, const EngineFacade *const &engine,
                  PreviewService &preview_service) noexcept;

    const std::unique_ptr<CatalogRepository> &repository_;
    const std::unique_ptr<RasterDecoder> &raster_;
    const EngineFacade *const &engine_;
    PreviewService &preview_service_;
};

} // namespace ravo
