#pragma once

#include <cstdint>
#include <string>

#include "ravo/foundation/cancellation.h"

namespace ravo
{

// Owner-thread identity and cancellation only. Scheduling, bounds, publication
// and commit policy remain with each consumer. begin() does not advance identity.
class CancellationGeneration
{
public:
    [[nodiscard]] std::uint64_t invalidate(std::string reason);
    [[nodiscard]] CancellationToken begin();
    void cancel(std::string reason);
    [[nodiscard]] CancellationToken token() const;
    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] bool accepts(std::uint64_t revision) const noexcept;

private:
    std::uint64_t revision_ = 0;
    CancellationSource active_;
};

} // namespace ravo
