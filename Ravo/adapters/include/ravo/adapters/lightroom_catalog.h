#pragma once

#include <string_view>
#include <vector>

#include "ravo/domain/foreign_catalog.h"
#include "ravo/foundation/cancellation.h"

namespace ravo
{
// Synchronous, caller-thread reader. Borrows path only for the call. A private
// snapshot and its read-only SQL connection are destroyed before return.
[[nodiscard]] Result<std::vector<ForeignCatalogPhoto>>
read_lightroom_catalog(std::string_view path, const CancellationToken &cancellation = {});
} // namespace ravo
