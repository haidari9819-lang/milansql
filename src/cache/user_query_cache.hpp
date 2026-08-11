#pragma once
// ============================================================
// user_query_cache.hpp — MilanSQL Per-User Query Result Cache (Phase 2.2)
// LRU eviction, per-user namespace, configurable max entries
// Keyed by: user + normalized_sql
// Invalidated on INSERT/UPDATE/DELETE/DDL for affected table
// Thread-safe (mutex)
// ============================================================

#include <string>
#include <unordered_map>
#include <list>
#include <mutex>
#include <optional>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <iostream>
#include <sstream>

namespace milansql {

// ── Normalize SQL for cache key ───────────────────────────────
inline std::string normalizeSql(const std::string& sql) {
    // Uppercase + collapse whitespace
    std::string result;
    result.reserve(sql.size());
    bool lastWasSpace = false;
    for (unsigned char c : sql) {
        if (std::isspace(c)) {
            if (!lastWasSpace && !result.empty()) {
                result += ' ';
                lastWasSpace = true;
            }
        } else {
            result += static_cast<char>(std::toupper(c));
            lastWasSpace = false;
        }
    }
    // Remove trailing space
    while (!result.empty() && result.back() == ' ') result.pop_back();
    // Remove trailing semicolon
    while (!result.empty() && result.back() == ';') result.pop_back();
    return result;
}

struct UserCacheEntry {
    std::string result;       // cached JSON/text result
    std::string tableName;    // primary table this query touches (for invalidation)
    std::chrono::steady_clock::time_point cachedAt;
};

class UserQueryCache {
public:
    UserQueryCache() = default;

    // ── get ───────────────────────────────────────────────────
    std::optional<std::string> get(const std::string& user, const std::string& sql) {
        if (!enabled_) return std::nullopt;
        std::lock_guard<std::mutex> lk(mutex_);
        std::string key = makeKey(user, sql);
        auto it = index_.find(key);
        if (it == index_.end()) {
            ++misses_;
            return std::nullopt;
        }
        // TTL check (30 seconds)
        auto age = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - it->second->cachedAt).count();
        if (age > 30) {
            lru_.erase(it->second);
            index_.erase(it);
            ++misses_;
            return std::nullopt;
        }
        // Move to front (most recently used)
        lru_.splice(lru_.begin(), lru_, it->second);
        ++hits_;
        return it->second->result;
    }

    // ── put ───────────────────────────────────────────────────
    void put(const std::string& user, const std::string& sql,
             const std::string& result, const std::string& tableName) {
        if (!enabled_) return;
        std::lock_guard<std::mutex> lk(mutex_);
        std::string key = makeKey(user, sql);

        // If already exists, update
        auto it = index_.find(key);
        if (it != index_.end()) {
            lru_.erase(it->second);
            index_.erase(it);
        }

        // Evict LRU if at capacity
        while (lru_.size() >= maxEntries_) {
            auto& back = lru_.back();
            index_.erase(back.key);
            lru_.pop_back();
        }

        lru_.push_front({key, result, tableName, std::chrono::steady_clock::now()});
        index_[key] = lru_.begin();
    }

    // ── invalidate by table ───────────────────────────────────
    // Remove all entries whose tableName matches (case-insensitive)
    void invalidate(const std::string& tableName) {
        std::lock_guard<std::mutex> lk(mutex_);
        std::string upper = tableName;
        for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        for (auto it = lru_.begin(); it != lru_.end(); ) {
            std::string tbl = it->tableName;
            for (auto& c : tbl) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (tbl == upper) {
                index_.erase(it->key);
                it = lru_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // ── flush all ─────────────────────────────────────────────
    void flush() {
        std::lock_guard<std::mutex> lk(mutex_);
        lru_.clear();
        index_.clear();
    }

    // ── configuration ─────────────────────────────────────────
    void setMaxEntries(size_t n) {
        std::lock_guard<std::mutex> lk(mutex_);
        maxEntries_ = (n == 0) ? 1 : n;
        // Evict if over limit
        while (lru_.size() > maxEntries_) {
            auto& back = lru_.back();
            index_.erase(back.key);
            lru_.pop_back();
        }
    }
    void setEnabled(bool v) { enabled_ = v; }
    bool isEnabled() const  { return enabled_; }

    // ── metrics ───────────────────────────────────────────────
    long long hits()    const { return hits_.load(); }
    long long misses()  const { return misses_.load(); }
    size_t    size()    const { std::lock_guard<std::mutex> lk(mutex_); return lru_.size(); }
    size_t    maxSize() const { return maxEntries_; }

    void resetCounters() { hits_ = 0; misses_ = 0; }

    std::string statsJson() const {
        std::ostringstream ss;
        ss << "{\"enabled\":" << (enabled_ ? "true" : "false")
           << ",\"size\":" << lru_.size()
           << ",\"max_entries\":" << maxEntries_
           << ",\"hits\":" << hits_.load()
           << ",\"misses\":" << misses_.load()
           << "}";
        return ss.str();
    }

private:
    struct LRUNode {
        std::string key;
        std::string result;
        std::string tableName;
        std::chrono::steady_clock::time_point cachedAt;
    };

    static std::string makeKey(const std::string& user, const std::string& sql) {
        return user + '\x01' + normalizeSql(sql);
    }

    using LRUList = std::list<LRUNode>;
    using LRUIndex = std::unordered_map<std::string, LRUList::iterator>;

    mutable std::mutex mutex_;
    LRUList            lru_;
    LRUIndex           index_;
    size_t             maxEntries_ = 256;
    bool               enabled_    = true;
    std::atomic<long long> hits_{0};
    std::atomic<long long> misses_{0};
};

// ── Global singleton ──────────────────────────────────────────
inline UserQueryCache& g_userQueryCache() {
    static UserQueryCache cache;
    return cache;
}

} // namespace milansql
