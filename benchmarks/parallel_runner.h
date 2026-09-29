#pragma once

#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

// 创建线程后统一起跑；线程内异常先保存，join 全部线程后在调用者中重抛。
// 计时包含唤醒和工作完成，不包含线程创建、join；每次操作不经过共享计数器。
template<class Function>
double RunThreads(size_t count, Function function) {
    if (count == 0) throw std::invalid_argument("thread count must be positive");
    using Clock = std::chrono::steady_clock;
    std::mutex mutex;
    std::condition_variable cv;
    size_t ready = 0, finished = 0;
    bool start = false, cancel = false;
    Clock::time_point begin, end;
    std::vector<std::exception_ptr> errors(count);
    std::vector<std::thread> workers;
    workers.reserve(count);
    try {
        for (size_t t = 0; t < count; ++t) workers.emplace_back([&, t] {
            {
                std::unique_lock<std::mutex> lock(mutex);
                ++ready;
                cv.notify_all();
                cv.wait(lock, [&] { return start; });
                if (cancel) return;
            }
            try { function(t); }
            catch (...) { errors[t] = std::current_exception(); }
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (++finished == count) end = Clock::now();
                cv.notify_all();
            }
        });
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            cancel = true;
            start = true;
        }
        cv.notify_all();
        for (auto& worker : workers) worker.join();
        throw;
    }
    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] { return ready == count; });
        begin = Clock::now();
        start = true;
        cv.notify_all();
        cv.wait(lock, [&] { return finished == count; });
    }
    for (auto& worker : workers) worker.join();
    for (const auto& error : errors) if (error) std::rethrow_exception(error);
    return std::chrono::duration<double, std::milli>(end - begin).count();
}
