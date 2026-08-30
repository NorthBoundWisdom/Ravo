#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>
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

class ParallelRowSession final
{
public:
    ParallelRowSession(const ParallelRowSession &) = delete;
    ParallelRowSession &operator=(const ParallelRowSession &) = delete;

    ~ParallelRowSession()
    {
        {
            const std::lock_guard lock(mutex_);
            stopping_ = true;
        }
        start_cv_.notify_all();
        for (auto &thread : threads_)
        {
            thread.join();
        }
    }

    [[nodiscard]] static Result<std::unique_ptr<ParallelRowSession>> create()
    try
    {
        auto session = std::unique_ptr<ParallelRowSession>(new ParallelRowSession());
        session->threads_.reserve(session->worker_capacity_ - 1U);
        try
        {
            for (unsigned worker = 1U; worker < session->worker_capacity_; ++worker)
            {
                session->threads_.emplace_back(&ParallelRowSession::worker_loop, session.get(),
                                               worker);
            }
        }
        catch (const std::system_error &error)
        {
            {
                const std::lock_guard lock(session->mutex_);
                session->stopping_ = true;
            }
            session->start_cv_.notify_all();
            for (auto &thread : session->threads_)
            {
                thread.join();
            }
            session->threads_.clear();
            return make_error(ErrorCode::kIo, "Unable to start a parallel row worker",
                              {{"reason", error.code().message()}});
        }
        return session;
    }
    catch (const std::bad_alloc &)
    {
        return make_error(ErrorCode::kIo, "Unable to allocate a parallel row session",
                          {{"reason", "allocation_failed"}});
    }

    template <typename Fn>
    Result<void> run(const std::uint32_t rows, const CancellationToken &cancellation, Fn &&fn)
    {
        auto active = cancellation.check();
        if (!active)
        {
            return active.error();
        }
        const unsigned workers = std::min(worker_capacity_, parallel_row_worker_bound(rows));
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
            const std::uint32_t end = static_cast<std::uint32_t>(static_cast<std::uint64_t>(rows) *
                                                                 (worker + 1U) / workers);
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

        try
        {
            {
                const std::lock_guard lock(mutex_);
                active_workers_ = workers;
                completed_workers_ = 0U;
                job_ = run_range;
                ++generation_;
            }
        }
        catch (const std::bad_alloc &)
        {
            return make_error(ErrorCode::kIo, "Unable to allocate a parallel row job",
                              {{"reason", "allocation_failed"}});
        }
        start_cv_.notify_all();
        run_range(0U);
        {
            std::unique_lock lock(mutex_);
            done_cv_.wait(lock, [&] { return completed_workers_ == workers - 1U; });
            job_ = {};
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

private:
    ParallelRowSession()
        : worker_capacity_(parallel_row_workers(std::numeric_limits<std::uint32_t>::max()))
    {
    }

    void worker_loop(const unsigned worker)
    {
        std::uint64_t observed_generation = 0U;
        for (;;)
        {
            std::unique_lock lock(mutex_);
            start_cv_.wait(lock, [&] { return stopping_ || generation_ != observed_generation; });
            if (stopping_)
            {
                return;
            }
            observed_generation = generation_;
            if (worker >= active_workers_)
            {
                continue;
            }
            lock.unlock();
            job_(worker);
            lock.lock();
            ++completed_workers_;
            if (completed_workers_ == active_workers_ - 1U)
            {
                done_cv_.notify_one();
            }
        }
    }

    const unsigned worker_capacity_;
    std::mutex mutex_;
    std::condition_variable start_cv_;
    std::condition_variable done_cv_;
    std::vector<std::thread> threads_;
    std::function<void(unsigned)> job_;
    std::uint64_t generation_ = 0U;
    unsigned active_workers_ = 1U;
    unsigned completed_workers_ = 0U;
    bool stopping_ = false;
};

inline thread_local ParallelRowSession *active_parallel_row_session = nullptr;

class ScopedParallelRowSession final
{
public:
    ScopedParallelRowSession(const ScopedParallelRowSession &) = delete;
    ScopedParallelRowSession &operator=(const ScopedParallelRowSession &) = delete;

    ScopedParallelRowSession(ScopedParallelRowSession &&other) noexcept
        : session_(std::move(other.session_))
        , previous_(other.previous_)
        , active_(std::exchange(other.active_, false))
    {
    }

    ~ScopedParallelRowSession()
    {
        if (active_)
        {
            active_parallel_row_session = previous_;
        }
    }

    [[nodiscard]] static Result<ScopedParallelRowSession> create()
    {
        if (active_parallel_row_session != nullptr)
        {
            return ScopedParallelRowSession(nullptr, active_parallel_row_session);
        }
        auto session = ParallelRowSession::create();
        if (!session)
        {
            return session.error();
        }
        return ScopedParallelRowSession(std::move(session).value(), nullptr);
    }

    [[nodiscard]] static ScopedParallelRowSession borrow(ParallelRowSession &session) noexcept
    {
        if (active_parallel_row_session != nullptr)
        {
            return ScopedParallelRowSession(nullptr, active_parallel_row_session);
        }
        return ScopedParallelRowSession(nullptr, nullptr, &session);
    }

private:
    ScopedParallelRowSession(std::unique_ptr<ParallelRowSession> session,
                             ParallelRowSession *previous,
                             ParallelRowSession *borrowed = nullptr) noexcept
        : session_(std::move(session))
        , previous_(previous)
    {
        active_parallel_row_session = session_ != nullptr ? session_.get() :
                                      borrowed != nullptr ? borrowed :
                                                            previous_;
    }

    std::unique_ptr<ParallelRowSession> session_;
    ParallelRowSession *previous_ = nullptr;
    bool active_ = true;
};

// Pixel rows have uniform cost in the current CPU operations. Static contiguous
// ranges avoid a shared atomic row queue, while the calling render thread owns
// one range instead of idling until temporary workers join.
template <typename Fn>
Result<void> for_each_row(const std::uint32_t rows, const CancellationToken &cancellation, Fn &&fn)
{
    if (active_parallel_row_session != nullptr)
    {
        return active_parallel_row_session->run(rows, cancellation, std::forward<Fn>(fn));
    }
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
