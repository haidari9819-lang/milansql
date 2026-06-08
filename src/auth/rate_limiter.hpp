#pragma once
// ============================================================
// rate_limiter.hpp — Token Bucket Rate Limiter (Phase 154)
// ============================================================
#include <string>
#include <map>
#include <mutex>
#include <chrono>

class RateLimiter {
public:
    RateLimiter(int capacity, double refillPerSecond)
        : capacity_(capacity), refillPerSecond_(refillPerSecond) {}

    bool allow(const std::string& key) {
        std::lock_guard<std::mutex> lk(mutex_);
        auto now = std::chrono::steady_clock::now();
        auto& b = buckets_[key];
        if (b.lastRefill == std::chrono::steady_clock::time_point{}) {
            b.tokens = (double)capacity_;
            b.lastRefill = now;
        } else {
            double secs = std::chrono::duration<double>(now - b.lastRefill).count();
            b.tokens = std::min((double)capacity_, b.tokens + secs * refillPerSecond_);
            b.lastRefill = now;
        }
        if (b.tokens < 1.0) return false;
        b.tokens -= 1.0;
        b.count++;
        return true;
    }

    int requestCount(const std::string& key) const {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = buckets_.find(key);
        return (it != buckets_.end()) ? it->second.count : 0;
    }

private:
    struct Bucket {
        double tokens = 0.0;
        std::chrono::steady_clock::time_point lastRefill{};
        int count = 0;
    };
    int capacity_;
    double refillPerSecond_;
    std::map<std::string, Bucket> buckets_;
    mutable std::mutex mutex_;
};
