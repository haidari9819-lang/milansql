#pragma once
// ============================================================
// monitoring/metrics.hpp — Thread-safe atomic counters
// Phase 1.1: Prometheus Metrics (Billion-Scale Roadmap)
// ============================================================

#include <atomic>
#include <chrono>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <numeric>
#include <algorithm>
#include <array>
#include <cstdint>

namespace milansql {

struct MetricsCollector {
    // Queries
    std::atomic<uint64_t> queries_select{0};
    std::atomic<uint64_t> queries_insert{0};
    std::atomic<uint64_t> queries_update{0};
    std::atomic<uint64_t> queries_delete{0};
    std::atomic<uint64_t> slow_queries_total{0};  // >100ms

    // Connections
    std::atomic<uint64_t> connections_active{0};
    std::atomic<uint64_t> connections_total{0};
    std::atomic<uint64_t> connections_rejected{0};

    // Cache
    std::atomic<uint64_t> buffer_hits{0};
    std::atomic<uint64_t> buffer_misses{0};

    // Storage
    std::atomic<uint64_t> disk_reads{0};
    std::atomic<uint64_t> disk_writes{0};

    // Query timing — simple ring buffer for p50/p95/p99
    static constexpr size_t TIMING_BUF = 1024;
    std::array<double, TIMING_BUF> timing_buf{};
    std::atomic<size_t> timing_idx{0};
    mutable std::mutex timing_mu;

    void record_duration(double ms) {
        std::lock_guard<std::mutex> lk(timing_mu);
        timing_buf[timing_idx.load() % TIMING_BUF] = ms;
        timing_idx.fetch_add(1, std::memory_order_relaxed);
        if (ms > 100.0) slow_queries_total.fetch_add(1, std::memory_order_relaxed);
    }

    // Returns {p50, p95, p99} in seconds
    std::array<double, 3> quantiles() const {
        std::vector<double> v;
        v.reserve(TIMING_BUF);
        {
            size_t idx = timing_idx.load();
            size_t n = std::min(idx, TIMING_BUF);
            for (size_t i = 0; i < n; ++i)
                v.push_back(timing_buf[i] / 1000.0);  // ms -> s
        }
        if (v.empty()) return {0.0, 0.0, 0.0};
        std::sort(v.begin(), v.end());
        auto pct = [&](double p) -> double {
            return v[static_cast<size_t>(p * static_cast<double>(v.size() - 1))];
        };
        return { pct(0.5), pct(0.95), pct(0.99) };
    }

    double hit_ratio() const {
        uint64_t h = buffer_hits.load(), m = buffer_misses.load();
        return (h + m > 0) ? static_cast<double>(h) / static_cast<double>(h + m) : 0.0;
    }

    // Startup time
    std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();

    double uptime_seconds() const {
        return std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start_time).count();
    }

    static MetricsCollector& global() {
        static MetricsCollector inst;
        return inst;
    }
};

} // namespace milansql
