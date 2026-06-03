#pragma once
// Built-in memory usage tracker (does NOT override global new/delete to avoid conflicts)
// Instead, tracks engine-level allocation metrics via counters.

#include <atomic>
#include <string>
#include <cstdint>

namespace milansql {

struct MemoryStats {
    int64_t allocatedObjects = 0;
    int64_t allocatedBytes = 0;
    int64_t peakBytes = 0;
    int64_t leaks = 0;
};

// Lightweight tracker — manually updated by engine components
class MemoryTracker {
public:
    void recordAlloc(int64_t bytes) {
        allocatedObjects_++;
        allocatedBytes_ += bytes;
        int64_t cur = allocatedBytes_.load();
        int64_t peak = peakBytes_.load();
        while (cur > peak && !peakBytes_.compare_exchange_weak(peak, cur)) {}
    }

    void recordFree(int64_t bytes) {
        allocatedObjects_--;
        allocatedBytes_ -= bytes;
    }

    MemoryStats stats() const {
        MemoryStats s;
        s.allocatedObjects = allocatedObjects_.load();
        s.allocatedBytes   = allocatedBytes_.load();
        s.peakBytes        = peakBytes_.load();
        s.leaks            = std::max(int64_t(0), allocatedObjects_.load());
        return s;
    }

    void reset() {
        allocatedObjects_ = 0;
        allocatedBytes_   = 0;
        peakBytes_        = 0;
    }

    static MemoryTracker& global() {
        static MemoryTracker instance;
        return instance;
    }

private:
    std::atomic<int64_t> allocatedObjects_{0};
    std::atomic<int64_t> allocatedBytes_{0};
    std::atomic<int64_t> peakBytes_{0};
};

} // namespace milansql
