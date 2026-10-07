#include "ravo/desktop/preview_request_owner.h"

#include <utility>

namespace ravo
{

std::uint64_t PreviewRequestOwner::supersede(std::string reason)
{
    return generation_.invalidate(std::move(reason));
}

std::uint64_t PreviewRequestOwner::revision() const noexcept
{
    return generation_.revision();
}

CancellationToken PreviewRequestOwner::begin()
{
    return generation_.begin();
}

bool PreviewRequestOwner::accepts(const std::uint64_t revision,
                                  const std::string_view result_asset_id,
                                  const std::string_view selected_asset_id) const noexcept
{
    return generation_.accepts(revision) && result_asset_id == selected_asset_id;
}

void PreviewRequestOwner::cancel(std::string reason)
{
    generation_.cancel(std::move(reason));
}

} // namespace ravo
