#include "ravo/foundation/executor.h"

#include <cstdint>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace ravo
{
namespace
{

constexpr std::size_t kWorkerStackBytes = 8U * 1024U * 1024U;

extern "C" void *ravo_serial_executor_entry(void *argument)
{
    auto *body = static_cast<std::function<void()> *>(argument);
    (*body)();
    delete body;
    return nullptr;
}

#if defined(_WIN32)
unsigned __stdcall ravo_serial_executor_win_entry(void *argument)
{
    ravo_serial_executor_entry(argument);
    return 0;
}
#endif

} // namespace

SerialExecutor::SerialExecutor()
{
    start_worker();
}

void SerialExecutor::start_worker()
{
    std::promise<void> ready;
    auto started = ready.get_future();
    auto *body = new std::function<void()>([this, &ready]() {
        worker_id_ = std::this_thread::get_id();
        ready.set_value();
        run_loop();
    });

#if defined(_WIN32)
    const auto handle = _beginthreadex(nullptr, static_cast<unsigned>(kWorkerStackBytes),
                                       ravo_serial_executor_win_entry, body, 0, nullptr);
    if (handle == 0)
    {
        delete body;
        throw std::runtime_error("SerialExecutor failed to start a worker thread");
    }
    thread_ = reinterpret_cast<void *>(handle);
    thread_started_ = true;
#else
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, kWorkerStackBytes);
    pthread_t thread{};
    const int created = pthread_create(&thread, &attributes, ravo_serial_executor_entry, body);
    pthread_attr_destroy(&attributes);
    if (created != 0)
    {
        delete body;
        throw std::runtime_error("SerialExecutor failed to start a worker thread");
    }
    thread_ = reinterpret_cast<void *>(thread);
    thread_started_ = true;
#endif

    started.wait();
}

SerialExecutor::~SerialExecutor()
{
    request_stop();
    wait();
}

bool SerialExecutor::post(std::function<void()> task)
{
    {
        std::lock_guard lock(mutex_);
        if (stop_)
        {
            return false;
        }
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
    return true;
}

void SerialExecutor::request_stop()
{
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
}

void SerialExecutor::wait_idle()
{
    std::unique_lock lock(mutex_);
    idle_cv_.wait(lock, [this]() { return tasks_.empty() && !running_; });
}

void SerialExecutor::wait()
{
#if defined(_WIN32)
    if (thread_started_ && thread_ != nullptr)
    {
        WaitForSingleObject(static_cast<HANDLE>(thread_), INFINITE);
        CloseHandle(static_cast<HANDLE>(thread_));
        thread_ = nullptr;
        thread_started_ = false;
    }
#else
    if (thread_started_ && thread_ != nullptr)
    {
        pthread_join(reinterpret_cast<pthread_t>(thread_), nullptr);
        thread_ = nullptr;
        thread_started_ = false;
    }
#endif
}

bool SerialExecutor::is_stop_requested() const noexcept
{
    std::lock_guard lock(mutex_);
    return stop_;
}

bool SerialExecutor::is_worker_thread() const noexcept
{
    return std::this_thread::get_id() == worker_id_;
}

std::thread::id SerialExecutor::worker_thread_id() const noexcept
{
    return worker_id_;
}

void SerialExecutor::run_loop()
{
    for (;;)
    {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this]() { return stop_ || !tasks_.empty(); });
            if (stop_ && tasks_.empty())
            {
                running_ = false;
                idle_cv_.notify_all();
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
            running_ = true;
        }
        task();
        {
            std::lock_guard lock(mutex_);
            running_ = false;
            if (tasks_.empty())
            {
                idle_cv_.notify_all();
            }
        }
    }
}

} // namespace ravo
