#include "ravo/foundation/cancellation.h"

#include <atomic>
#include <array>
#include <chrono>
#include <mutex>
#include <optional>
#include <utility>

namespace ravo::detail
{

struct CancellationState
{
    std::atomic_bool requested = false;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    mutable std::mutex mutex;
    std::string reason;
    std::array<std::shared_ptr<CancellationState>, 2> parents{};
};

} // namespace ravo::detail

namespace ravo
{

CancellationToken::CancellationToken(std::shared_ptr<detail::CancellationState> state)
    : state_(std::move(state))
{
}

bool CancellationToken::is_cancellation_requested() const noexcept
{
    if (state_ == nullptr)
    {
        return false;
    }
    if (state_->requested.load(std::memory_order_acquire))
    {
        return true;
    }
    for (const auto &parent : state_->parents)
        if (parent && CancellationToken(parent).is_cancellation_requested())
            return true;
    return state_->deadline.has_value() && std::chrono::steady_clock::now() >= *state_->deadline;
}

std::string CancellationToken::reason() const
{
    if (state_ == nullptr)
    {
        return {};
    }
    for (const auto &parent : state_->parents)
        if (parent && CancellationToken(parent).is_cancellation_requested())
            return CancellationToken(parent).reason();
    if (!state_->requested.load(std::memory_order_acquire) && state_->deadline.has_value() &&
        std::chrono::steady_clock::now() >= *state_->deadline)
    {
        return "deadline_exceeded";
    }
    std::lock_guard lock(state_->mutex);
    return state_->reason;
}

Result<void> CancellationToken::check() const
{
    if (!is_cancellation_requested())
    {
        return {};
    }

    return make_error(ErrorCode::kCancelled, "Ravo task was cancelled", {{"reason", reason()}});
}

CancellationToken CancellationToken::combine(const CancellationToken &first,
                                             const CancellationToken &second)
{
    if (!first.state_)
        return second;
    if (!second.state_)
        return first;
    auto state = std::make_shared<detail::CancellationState>();
    state->parents = {first.state_, second.state_};
    return CancellationToken{std::move(state)};
}

CancellationSource::CancellationSource()
    : state_(std::make_shared<detail::CancellationState>())
{
}

CancellationSource::CancellationSource(const std::chrono::steady_clock::time_point deadline)
    : CancellationSource()
{
    state_->deadline = deadline;
}

CancellationSource
CancellationSource::with_deadline(const std::chrono::steady_clock::time_point deadline)
{
    return CancellationSource{deadline};
}

CancellationToken CancellationSource::token() const
{
    return CancellationToken{state_};
}

bool CancellationSource::cancel(std::string reason)
{
    std::lock_guard lock(state_->mutex);
    bool expected = false;
    if (!state_->requested.compare_exchange_strong(expected, true, std::memory_order_release,
                                                   std::memory_order_relaxed))
    {
        return false;
    }

    state_->reason = std::move(reason);
    return true;
}

} // namespace ravo
