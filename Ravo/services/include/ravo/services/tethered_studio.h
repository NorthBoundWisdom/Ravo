#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "ravo/foundation/error.h"

namespace ravo
{

// ADR-0153: SPECIALIZE-01 deferred tethered-studio probe (fail-closed).
inline constexpr std::string_view kTetheredStudioContractVersion = "ravo.specialize.tethered/v1";
inline constexpr std::int64_t kTetheredStudioSchemaVersion = 1;

struct TetheredStudioProbeStatus
{
    std::string schema{std::string(kTetheredStudioContractVersion)};
    std::int64_t schema_version = kTetheredStudioSchemaVersion;
    bool supported = false;
    std::string reason{"tethered_deferred"};
    std::string track{"tethered_studio"};
    std::string priority{"P2_deferred"};
};

// Always fail-closed until a later ADR admits a real adapter.
[[nodiscard]] Result<TetheredStudioProbeStatus> probe_tethered_studio_support();

} // namespace ravo
