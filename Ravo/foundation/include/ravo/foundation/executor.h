#pragma once

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace ravo
{

// Single owned worker. Callers must not post after wait() and must not detach work.
class SerialExecutor
{
public:
    SerialExecutor();
    SerialExecutor(const SerialExecutor &) = delete;
    SerialExecutor &operator=(const SerialExecutor &) = delete;
    ~SerialExecutor();

    bool post(std::function<void()> task);
    void request_stop();
    void wait_idle();
    void wait();

    [[nodiscard]] bool is_stop_requested() const noexcept;
    [[nodiscard]] bool is_worker_thread() const noexcept;
    [[nodiscard]] std::thread::id worker_thread_id() const noexcept;

    template <typename F>
    auto submit(F &&function) -> std::invoke_result_t<F>
    {
        using ResultType = std::invoke_result_t<F>;
        if (is_worker_thread())
        {
            return function();
        }

        std::promise<ResultType> promise;
        auto future = promise.get_future();
        const bool queued = post(
            [&function, &promise]()
            {
                try
                {
                    if constexpr (std::is_void_v<ResultType>)
                    {
                        function();
                        promise.set_value();
                    }
                    else
                    {
                        promise.set_value(function());
                    }
                }
                catch (...)
                {
                    promise.set_exception(std::current_exception());
                }
            });
        if (!queued)
        {
            throw std::runtime_error("SerialExecutor has stopped");
        }
        return future.get();
    }

private:
    void run_loop();
    void start_worker();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::condition_variable idle_cv_;
    std::queue<std::function<void()>> tasks_;
    bool stop_ = false;
    bool running_ = false;
    std::thread::id worker_id_{};
    void *thread_ = nullptr;
    bool thread_started_ = false;
};

} // namespace ravo
