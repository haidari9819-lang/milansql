#pragma once
// ============================================================
// rate_limiter.hpp — Tiered Token Bucket Rate Limiter
// Phase 154 → Phase 167: Production Tiered Rate Limiting
// ============================================================
#include <string>
#include <map>
#include <mutex>
#include <chrono>

// ── Rate Limit Tiers ────────────────────────────────────────
enum class RateTier {
    ANONYMOUS,   // unauthenticated / unknown
    FREE,        // free-tier users
    PRO,         // paid users
    API_KEY,     // server-to-server API keys
    ADMIN        // administrators (unlimited)
};

struct TierConfig {
    int    capacity;         // burst capacity (max tokens)
    double refillPerSecond;  // sustained rate (tokens/sec)

    static TierConfig forTier(RateTier tier) {
        switch (tier) {
            case RateTier::ANONYMOUS: return {  60,   60.0/60.0};  //   60/min
            case RateTier::FREE:     return { 600,  600.0/60.0};  //  600/min (10x anonymous)
            case RateTier::PRO:      return {5000, 5000.0/60.0};  // 5000/min
            case RateTier::API_KEY:  return {10000,10000.0/60.0}; //10000/min
            case RateTier::ADMIN:    return {100000,100000.0/60.0}; // effectively unlimited
        }
        return {100, 100.0/60.0};
    }
};

// ── Rate Limiter ────────────────────────────────────────────
class RateLimiter {
public:
    // Legacy constructor: single capacity/rate for all keys
    RateLimiter(int capacity, double refillPerSecond)
        : defaultCapacity_(capacity), defaultRefill_(refillPerSecond),
          tieredMode_(false) {}

    // Tiered constructor: per-key tier-based limits
    RateLimiter() : defaultCapacity_(100), defaultRefill_(100.0/60.0),
                    tieredMode_(true) {}

    // Set a key's tier (call when user authenticates)
    void setTier(const std::string& key, RateTier tier) {
        std::lock_guard<std::mutex> lk(mutex_);
        keyTiers_[key] = tier;
        // Reset bucket to new tier's capacity
        auto cfg = TierConfig::forTier(tier);
        auto& b = buckets_[key];
        b.tokens = (double)cfg.capacity;
        b.lastRefill = std::chrono::steady_clock::now();
    }

    RateTier getTier(const std::string& key) const {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = keyTiers_.find(key);
        return (it != keyTiers_.end()) ? it->second : RateTier::ANONYMOUS;
    }

    bool allow(const std::string& key) {
        std::lock_guard<std::mutex> lk(mutex_);

        // ADMIN tier: always allow, no token accounting
        if (tieredMode_) {
            auto tierIt = keyTiers_.find(key);
            if (tierIt != keyTiers_.end() && tierIt->second == RateTier::ADMIN) {
                buckets_[key].count++;
                return true;
            }
        }

        auto now = std::chrono::steady_clock::now();

        int cap;
        double refill;
        if (tieredMode_) {
            auto tierIt = keyTiers_.find(key);
            RateTier tier = (tierIt != keyTiers_.end()) ? tierIt->second : RateTier::ANONYMOUS;
            auto cfg = TierConfig::forTier(tier);
            cap = cfg.capacity;
            refill = cfg.refillPerSecond;
        } else {
            cap = defaultCapacity_;
            refill = defaultRefill_;
        }

        auto& b = buckets_[key];
        if (b.lastRefill == std::chrono::steady_clock::time_point{}) {
            b.tokens = (double)cap;
            b.lastRefill = now;
        } else {
            double secs = std::chrono::duration<double>(now - b.lastRefill).count();
            b.tokens = std::min((double)cap, b.tokens + secs * refill);
            b.lastRefill = now;
        }
        if (b.tokens < 1.0) return false;
        b.tokens -= 1.0;
        b.count++;
        return true;
    }

    // How many tokens remain for a key
    double remaining(const std::string& key) const {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = buckets_.find(key);
        return (it != buckets_.end()) ? it->second.tokens : 0.0;
    }

    // How many seconds until next token
    double retryAfterSeconds(const std::string& key) const {
        std::lock_guard<std::mutex> lk(mutex_);
        double refill = defaultRefill_;
        if (tieredMode_) {
            auto tierIt = keyTiers_.find(key);
            RateTier tier = (tierIt != keyTiers_.end()) ? tierIt->second : RateTier::ANONYMOUS;
            refill = TierConfig::forTier(tier).refillPerSecond;
        }
        if (refill <= 0.0) return 60.0;
        return 1.0 / refill;
    }

    int requestCount(const std::string& key) const {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = buckets_.find(key);
        return (it != buckets_.end()) ? it->second.count : 0;
    }

    // Info string for rate limit headers
    std::string limitInfo(const std::string& key) const {
        std::lock_guard<std::mutex> lk(mutex_);
        int cap = defaultCapacity_;
        if (tieredMode_) {
            auto tierIt = keyTiers_.find(key);
            RateTier tier = (tierIt != keyTiers_.end()) ? tierIt->second : RateTier::ANONYMOUS;
            cap = TierConfig::forTier(tier).capacity;
        }
        auto it = buckets_.find(key);
        int rem = (it != buckets_.end()) ? (int)it->second.tokens : cap;
        return std::to_string(cap) + "," + std::to_string(rem);
    }

private:
    struct Bucket {
        double tokens = 0.0;
        std::chrono::steady_clock::time_point lastRefill{};
        int count = 0;
    };
    int defaultCapacity_;
    double defaultRefill_;
    bool tieredMode_;
    std::map<std::string, Bucket> buckets_;
    std::map<std::string, RateTier> keyTiers_;
    mutable std::mutex mutex_;
};
