#include "ravo/desktop/preview_request_owner.h"

#include <utility>

namespace ravo
{

std::uint64_t PreviewRequestOwner::supersede(std::string reason)
{
    static_cast<void>(active_.cancel(std::move(reason)));
    return ++revision_;
}

std::uint64_t PreviewRequestOwner::revision() const noexcept
{
    return revision_;
}

CancellationToken PreviewRequestOwner::begin()
{
    active_ = CancellationSource{};
    return active_.token();
}

bool PreviewRequestOwner::accepts(const std::uint64_t revision,
                                  const std::string_view result_asset_id,
                                  const std::string_view selected_asset_id) const noexcept
{
    return revision == revision_ && result_asset_id == selected_asset_id;
}

void PreviewRequestOwner::cancel(std::string reason)
{
    static_cast<void>(active_.cancel(std::move(reason)));
}

} // namespace ravo
