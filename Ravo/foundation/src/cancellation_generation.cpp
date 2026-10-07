#include "ravo/foundation/cancellation_generation.h"

#include <utility>

namespace ravo
{

std::uint64_t CancellationGeneration::invalidate(std::string reason)
{
    static_cast<void>(active_.cancel(std::move(reason)));
    return ++revision_;
}

CancellationToken CancellationGeneration::begin()
{
    active_ = CancellationSource{};
    return active_.token();
}

void CancellationGeneration::cancel(std::string reason)
{
    static_cast<void>(active_.cancel(std::move(reason)));
}

CancellationToken CancellationGeneration::token() const
{
    return active_.token();
}

std::uint64_t CancellationGeneration::revision() const noexcept
{
    return revision_;
}

bool CancellationGeneration::accepts(const std::uint64_t revision) const noexcept
{
    return revision == revision_;
}

} // namespace ravo
