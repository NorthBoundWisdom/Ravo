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
#include "ravo/services/offline_edit_proxy.h"

namespace ravo
{
class CatalogService;
class CatalogRepository;
class RasterDecoder;
class EngineFacade;
class PreviewCache;
class RecoveryStore;
class ExportService;
class DevelopService;

// One owner-thread use-case service. Borrowed slots/capabilities outlive this
// object; closed resource slots preserve the existing structured failures.
class OfflineEditService
{
public:
    OfflineEditService(const OfflineEditService &) = delete;
    OfflineEditService &operator=(const OfflineEditService &) = delete;
    OfflineEditService(OfflineEditService &&) = delete;
    OfflineEditService &operator=(OfflineEditService &&) = delete;

    [[nodiscard]] Result<OfflineEditProxyCreateResult>
    create_offline_edit_proxy(const OfflineEditProxyCreateRequest &request);
    [[nodiscard]] Result<OfflineEditProxyListReport> list_offline_edit_proxies() const;
    [[nodiscard]] Result<OfflineEditProxyStatus>
    verify_offline_edit_proxy(std::string_view asset_id) const;
    [[nodiscard]] Result<OfflineEditProxyStatus>
    offline_edit_media_status(std::string_view asset_id) const;
    [[nodiscard]] Result<OfflineEditProxyReconnectResult>
    reconnect_offline_edit_proxy(const OfflineEditProxyReconnectRequest &request);
    [[nodiscard]] Result<OfflineEditProxyDeleteResult>
    delete_offline_edit_proxy(const OfflineEditProxyDeleteRequest &request);
    [[nodiscard]] Result<OfflineEditProxyPinResult>
    pin_offline_edit_proxy(const OfflineEditProxyPinRequest &request);
    [[nodiscard]] Result<OfflineEditProxyEvictResult>
    evict_offline_edit_proxies(const OfflineEditProxyEvictRequest &request);

private:
    friend class CatalogService;
    OfflineEditService(
        const std::unique_ptr<CatalogRepository> &repository,
        const std::unique_ptr<RasterDecoder> &raster, const EngineFacade *const &engine,
        const std::function<Result<void>(std::string_view, std::string_view)> &offline_checkpoint,
        ExportService &exports_service, DevelopService &develop_service) noexcept;

    const std::unique_ptr<CatalogRepository> &repository_;
    const std::unique_ptr<RasterDecoder> &raster_;
    const EngineFacade *const &engine_;
    const std::function<Result<void>(std::string_view, std::string_view)>
        &testing_before_offline_proxy_publish_;
    ExportService &exports_service_;
    DevelopService &develop_service_;
};

} // namespace ravo
