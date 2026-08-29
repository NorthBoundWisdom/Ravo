#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

#include "ravo/foundation/cancellation.h"
#include "ravo/foundation/error.h"

namespace ravo::detail
{

inline constexpr unsigned kParallelRowWorkerLimit = 16U;

[[nodiscard]] inline unsigned parallel_row_worker_bound(const std::uint32_t rows) noexcept
{
    if (rows < 32U)
    {
        return 1U;
    }
    return std::min(kParallelRowWorkerLimit, rows);
}

[[nodiscard]] inline unsigned parallel_row_workers(const std::uint32_t rows) noexcept
{
    const unsigned hardware = std::max(1U, std::thread::hardware_concurrency());
    return std::min(hardware, parallel_row_worker_bound(rows));
}

// Pixel rows have uniform cost in the current CPU operations. Static contiguous
// ranges avoid a shared atomic row queue, while the calling render thread owns
// one range instead of idling until temporary workers join.
template <typename Fn>
Result<void> for_each_row(const std::uint32_t rows, const CancellationToken &cancellation, Fn &&fn)
{
    auto active = cancellation.check();
    if (!active)
    {
        return active.error();
    }
    const unsigned workers = parallel_row_workers(rows);
    if (workers <= 1U)
    {
        for (std::uint32_t row = 0; row < rows; ++row)
        {
            active = cancellation.check();
            if (!active)
            {
                return active.error();
            }
            if constexpr (std::is_invocable_v<Fn &, std::uint32_t, unsigned>)
            {
                fn(row, 0U);
            }
            else
            {
                fn(row);
            }
        }
        return {};
    }

    std::atomic<bool> cancelled{false};
    const auto run_range = [&](const unsigned worker)
    {
        const std::uint32_t begin =
            static_cast<std::uint32_t>(static_cast<std::uint64_t>(rows) * worker / workers);
        const std::uint32_t end =
            static_cast<std::uint32_t>(static_cast<std::uint64_t>(rows) * (worker + 1U) / workers);
        for (std::uint32_t row = begin; row < end; ++row)
        {
            if (cancelled.load(std::memory_order_acquire))
            {
                return;
            }
            if (!cancellation.check())
            {
                cancelled.store(true, std::memory_order_release);
                return;
            }
            if constexpr (std::is_invocable_v<Fn &, std::uint32_t, unsigned>)
            {
                fn(row, worker);
            }
            else
            {
                fn(row);
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(workers - 1U);
    try
    {
        for (unsigned worker = 1U; worker < workers; ++worker)
        {
            threads.emplace_back(run_range, worker);
        }
    }
    catch (const std::system_error &error)
    {
        cancelled.store(true, std::memory_order_release);
        for (auto &thread : threads)
        {
            thread.join();
        }
        return make_error(ErrorCode::kIo, "Unable to start a parallel row worker",
                          {{"reason", error.code().message()}});
    }
    run_range(0U);
    for (auto &thread : threads)
    {
        thread.join();
    }
    if (cancelled.load(std::memory_order_acquire))
    {
        active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
    }
    return {};
}

} // namespace ravo::detail
