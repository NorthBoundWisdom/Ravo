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
#include "ravo/services/xmp_interchange.h"

namespace ravo
{
class CatalogService;
class CatalogRepository;
class RasterDecoder;
class EngineFacade;
class PreviewCache;
class RecoveryStore;
class DevelopService;
class LibraryService;
class MetadataService;

// One owner-thread use-case service. Borrowed slots/capabilities outlive this
// object; closed resource slots preserve the existing structured failures.
class XmpInterchangeService
{
public:
    XmpInterchangeService(const XmpInterchangeService &) = delete;
    XmpInterchangeService &operator=(const XmpInterchangeService &) = delete;
    XmpInterchangeService(XmpInterchangeService &&) = delete;
    XmpInterchangeService &operator=(XmpInterchangeService &&) = delete;

    [[nodiscard]] Result<XmpInterchangeStatus>
    xmp_interchange_status(std::string_view asset_id,
                           std::optional<std::string_view> sidecar_path = std::nullopt) const;
    [[nodiscard]] Result<XmpInterchangeImportResult>
    xmp_interchange_import(std::string_view asset_id, XmpInterchangeResolve resolve,
                           std::optional<std::string_view> sidecar_path = std::nullopt);
    [[nodiscard]] Result<XmpInterchangeExportResult>
    xmp_interchange_export(std::string_view asset_id, XmpInterchangeResolve resolve,
                           std::optional<std::string_view> sidecar_path = std::nullopt);

private:
    friend class CatalogService;
    XmpInterchangeService(const std::unique_ptr<CatalogRepository> &repository,
                          DevelopService &develop_service, LibraryService &library_service,
                          MetadataService &metadata_service) noexcept;

    const std::unique_ptr<CatalogRepository> &repository_;
    DevelopService &develop_service_;
    LibraryService &library_service_;
    MetadataService &metadata_service_;
};

} // namespace ravo
