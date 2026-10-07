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
#include "ravo/services/foreign_catalog.h"
#include "ravo/services/dng_smart_preview.h"

namespace ravo
{
class CatalogService;
class CatalogRepository;
class RasterDecoder;
class EngineFacade;
class PreviewCache;
class RecoveryStore;
class DevelopService;
class ImportService;
class LibraryService;
class MetadataService;

// One owner-thread use-case service. Borrowed slots/capabilities outlive this
// object; closed resource slots preserve the existing structured failures.
class ConversionService
{
public:
    ConversionService(const ConversionService &) = delete;
    ConversionService &operator=(const ConversionService &) = delete;
    ConversionService(ConversionService &&) = delete;
    ConversionService &operator=(ConversionService &&) = delete;

    [[nodiscard]] Result<ForeignCatalogConversionReport>
    convert_foreign_catalog(const ForeignCatalogConversionRequest &request);
    [[nodiscard]] Result<DngConversionResult>
    convert_asset_to_dng(const DngConversionRequest &request);
    [[nodiscard]] Result<SmartPreviewStatus> smart_preview_status(std::string_view asset_id) const;
    [[nodiscard]] Result<SmartPreviewStatus>
    ensure_smart_preview(const SmartPreviewEnsureRequest &request);

private:
    friend class CatalogService;
    ConversionService(const std::unique_ptr<CatalogRepository> &repository,
                      DevelopService &develop_service, ImportService &import_service,
                      LibraryService &library_service, MetadataService &metadata_service) noexcept;

    const std::unique_ptr<CatalogRepository> &repository_;
    DevelopService &develop_service_;
    ImportService &import_service_;
    LibraryService &library_service_;
    MetadataService &metadata_service_;
};

} // namespace ravo
