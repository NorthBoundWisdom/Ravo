#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "ravo/foundation/cancellation.h"

namespace ravo
{

// UI-thread owner for one in-flight Develop preview. Superseding a revision cancels the token
// already borrowed by the worker; completion is accepted only for the current asset/revision.
class PreviewRequestOwner
{
public:
    [[nodiscard]] std::uint64_t supersede(std::string reason);
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] CancellationToken begin();
    [[nodiscard]] bool accepts(std::uint64_t revision, std::string_view result_asset_id,
                               std::string_view selected_asset_id) const noexcept;
    void cancel(std::string reason);

private:
    std::uint64_t revision_ = 0;
    CancellationSource active_;
};

} // namespace ravo
