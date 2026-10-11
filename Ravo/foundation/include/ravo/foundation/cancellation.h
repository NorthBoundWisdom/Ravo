#pragma once

#include <chrono>
#include <memory>
#include <string>

#include "ravo/foundation/error.h"

namespace ravo
{

namespace detail
{
struct CancellationState;
}

class CancellationSource;

class CancellationToken
{
public:
    CancellationToken() = default;
    // Immutable union of two owner lifetimes; retains both states. If both
    // cancel, the first argument supplies the reported reason.
    [[nodiscard]] static CancellationToken combine(const CancellationToken &first,
                                                   const CancellationToken &second);

    [[nodiscard]] bool is_cancellation_requested() const noexcept;
    [[nodiscard]] std::string reason() const;
    [[nodiscard]] Result<void> check() const;

private:
    explicit CancellationToken(std::shared_ptr<detail::CancellationState> state);

    std::shared_ptr<detail::CancellationState> state_;

    friend class CancellationSource;
};

class CancellationSource
{
public:
    CancellationSource();

    [[nodiscard]] static CancellationSource
    with_deadline(std::chrono::steady_clock::time_point deadline);
    [[nodiscard]] CancellationToken token() const;
    [[nodiscard]] bool cancel(std::string reason = "cancelled");

private:
    explicit CancellationSource(std::chrono::steady_clock::time_point deadline);

    std::shared_ptr<detail::CancellationState> state_;
};

} // namespace ravo
