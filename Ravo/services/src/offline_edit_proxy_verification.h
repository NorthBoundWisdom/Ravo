#pragma once

#include <string_view>

#include "ravo/services/offline_edit_proxy.h"

namespace ravo
{
class CatalogRepository;

// Shared read-only verification; callers establish an open owner-thread session.
[[nodiscard]] Result<OfflineEditProxyStatus> verify_offline_proxy(CatalogRepository &repository,
                                                                  std::string_view asset_id);
} // namespace ravo
