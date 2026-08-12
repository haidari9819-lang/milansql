#pragma once
// ============================================================
// thread_pool.hpp — MilanSQL Fixed-size Thread Pool (Phase 2.1)
// Fixed-size worker pool with std::future-based task submission
// Configurable workers (default 4), graceful shutdown
// ============================================================

#include <vector>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <atomic>
#include <stdexcept>
#include <memory>

namespace milansql {

class ThreadPool {
public:
    explicit ThreadPool(size_t numWorkers = 4) : stop_(false), activeWorkers_(0) {
        workers_.reserve(numWorkers);
        for (size_t i = 0; i < numWorkers; ++i) {
            workers_.emplace_back([this] { workerLoop(); });
        }
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
    }

    // Disable copy
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Submit a task, returns std::future<T>
    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<typename std::invoke_result<F, Args...>::type>
    {
        using ReturnType = typename std::invoke_result<F, Args...>::type;

        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        std::future<ReturnType> fut = task->get_future();

        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            if (stop_) throw std::runtime_error("ThreadPool: submit on stopped pool");
            tasks_.push([task]() { (*task)(); });
        }
        cv_.notify_one();
        return fut;
    }

    // Number of worker threads
    size_t size() const { return workers_.size(); }

    // Number of currently active (busy) workers
    size_t activeWorkers() const { return activeWorkers_.load(); }

    // Resize pool (stops old workers, starts new ones)
    void resize(size_t numWorkers) {
        // Stop existing workers
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
        workers_.clear();
        // Clear task queue
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            while (!tasks_.empty()) tasks_.pop();
            stop_ = false;
            activeWorkers_ = 0;
        }
        // Start new workers
        workers_.reserve(numWorkers);
        for (size_t i = 0; i < numWorkers; ++i) {
            workers_.emplace_back([this] { workerLoop(); });
        }
    }

private:
    void workerLoop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(queueMutex_);
                cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
                if (stop_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            ++activeWorkers_;
            try {
                task();
            } catch (...) {
                // Exceptions propagate via future
            }
            --activeWorkers_;
        }
    }

    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex                        queueMutex_;
    std::condition_variable           cv_;
    bool                              stop_;
    std::atomic<size_t>               activeWorkers_;
};

// ── Global thread pool singleton ──────────────────────────────
inline ThreadPool& g_threadPool() {
    static ThreadPool pool(4);
    return pool;
}

// ── parallel_workers_active counter ──────────────────────────
inline std::atomic<long long>& g_parallelWorkersActive() {
    static std::atomic<long long> cnt{0};
    return cnt;
}

// ── Parallel table scan helper ────────────────────────────────
// Splits [0, totalRows) into N chunks and processes each in parallel.
// combiner: merges partial results into final result
// scanner:  processes rows [start, end) and returns partial result
template<typename Row, typename Result>
Result parallelScan(
    const std::vector<Row>& rows,
    size_t numWorkers,
    std::function<Result(const std::vector<Row>&, size_t, size_t)> scanner,
    std::function<Result(std::vector<Result>)> combiner)
{
    if (rows.empty() || numWorkers <= 1) {
        return scanner(rows, 0, rows.size());
    }

    size_t total = rows.size();
    size_t chunkSize = (total + numWorkers - 1) / numWorkers;

    std::vector<std::future<Result>> futures;
    futures.reserve(numWorkers);

    ++g_parallelWorkersActive();

    auto& pool = g_threadPool();
    for (size_t i = 0; i < numWorkers; ++i) {
        size_t start = i * chunkSize;
        if (start >= total) break;
        size_t end = std::min(start + chunkSize, total);
        futures.push_back(pool.submit(scanner, std::cref(rows), start, end));
    }

    std::vector<Result> partials;
    partials.reserve(futures.size());
    for (auto& f : futures) {
        partials.push_back(f.get());
    }

    --g_parallelWorkersActive();
    return combiner(std::move(partials));
}

} // namespace milansql
