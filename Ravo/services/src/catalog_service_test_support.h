#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>

#include "ravo/services/catalog_service.h"

namespace ravo::testing
{

class CatalogServiceTestControl
{
public:
    static void set_before_import_publication(CatalogService &service,
                                              std::function<void()> callback);
    static void set_before_preview_cache_publication(CatalogService &service,
                                                     std::function<void()> callback);
    static void set_import_checkpoint(
        CatalogService &service,
        std::function<Result<void>(std::string_view checkpoint, std::string_view path)> callback);
    static void set_backup_checkpoint(
        CatalogService &service,
        std::function<Result<void>(std::string_view checkpoint, std::string_view path)> callback);
    static void set_before_offline_proxy_publish(
        CatalogService &service,
        std::function<Result<void>(std::string_view final_root, std::string_view staging_root)>
            callback);
    [[nodiscard]] static std::array<std::optional<std::uint32_t>, 2>
    linear_working_max_edges(const CatalogService &service);
    [[nodiscard]] static std::optional<std::uint32_t>
    browse_linear_working_max_edge(const CatalogService &service);
    [[nodiscard]] static std::optional<std::uint64_t>
    roi_linear_working_generation(const CatalogService &service);
};

} // namespace ravo::testing
