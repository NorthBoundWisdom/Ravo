#include "ravo/services/tethered_studio.h"

namespace ravo
{

Result<TetheredStudioProbeStatus> probe_tethered_studio_support()
{
    return make_error(ErrorCode::kUnsupported, "Tethered studio is deferred (SPECIALIZE-01)",
                      {{"reason", "tethered_deferred"},
                       {"contract", std::string(kTetheredStudioContractVersion)},
                       {"track", "tethered_studio"},
                       {"priority", "P2_deferred"}});
}

} // namespace ravo
