#pragma once
// ============================================================
// query_cache.hpp — MilanSQL Query Result Cache (Phase 54A)
// LRU eviction, 30s TTL, 100 entry max
// Stores rendered SELECT output (no dependency on engine types)
// ============================================================

#include <string>
#include <map>
#include <chrono>
#include <optional>
#include <iostream>
#include <algorithm>
#include <mutex>
#include <atomic>
#include <cstdint>

namespace milansql {

struct CacheEntry {
    std::string renderedOutput;   // captured cout text for this query
    std::string tableName;        // which table this query reads (for invalidation)
    std::chrono::steady_clock::time_point cachedAt;
    std::chrono::steady_clock::time_point lastAccess;
    size_t hitCount = 0;
};

class QueryCache {
public:
    QueryCache() = default;

    // Bug fix (read-after-write staleness / thread safety): this cache is shared
    // across the HTTP thread pool (every request runs dispatchCommand on a pool
    // thread) but originally had no locking at all around `cache_` -- concurrent
    // get()/put()/invalidate() from different threads is undefined behavior on a
    // std::map, and independent of that, a SELECT that read data just before a
    // concurrent write could still call put() *after* that write's invalidate()
    // already ran, silently re-caching a stale result for the full TTL. Fixed with
    // a mutex around all access plus a write-generation counter that put() checks.

    // ── get ───────────────────────────────────────────────────
    std::optional<std::string> get(const std::string& sql) {
        if (!enabled_) return std::nullopt;
        std::lock_guard<std::mutex> lock(mu_);
        auto it = cache_.find(sql);
        if (it == cache_.end()) { ++totalMisses_; return std::nullopt; }

        // TTL check
        auto now = std::chrono::steady_clock::now();
        auto age = std::chrono::duration_cast<std::chrono::seconds>(
                       now - it->second.cachedAt).count();
        if (age > ttlSeconds_) {
            cache_.erase(it);
            ++totalMisses_;
            return std::nullopt;
        }

        it->second.lastAccess = now;
        ++it->second.hitCount;
        ++totalHits_;
        return it->second.renderedOutput;
    }

    // Generation at the moment a query started reading, so put() can detect a
    // write that raced with it. Call this right before executing the query.
    uint64_t currentGeneration() const { return writeGen_.load(std::memory_order_acquire); }

    // ── put ───────────────────────────────────────────────────
    // genAtStart: value of currentGeneration() captured before this query ran.
    // If it no longer matches, some table was written (and invalidated) while we
    // were reading, so the result we're about to cache may already be stale -- skip.
    void put(const std::string& sql, const std::string& output,
             const std::string& tableName, uint64_t genAtStart) {
        if (!enabled_) return;
        std::lock_guard<std::mutex> lock(mu_);
        if (genAtStart != writeGen_.load(std::memory_order_acquire)) return;

        // LRU eviction if full: remove entry with oldest lastAccess
        if (cache_.size() >= maxSize_) {
            auto oldest = cache_.begin();
            for (auto it = std::next(cache_.begin()); it != cache_.end(); ++it) {
                if (it->second.lastAccess < oldest->second.lastAccess)
                    oldest = it;
            }
            cache_.erase(oldest);
        }

        auto now = std::chrono::steady_clock::now();
        CacheEntry e;
        e.renderedOutput = output;
        e.tableName      = tableName;
        e.cachedAt       = now;
        e.lastAccess     = now;
        e.hitCount       = 0;
        cache_[sql] = std::move(e);
    }

    // ── invalidate ────────────────────────────────────────────
    // Remove all entries that read the given table
    void invalidate(const std::string& tableName) {
        std::lock_guard<std::mutex> lock(mu_);
        writeGen_.fetch_add(1, std::memory_order_acq_rel);
        for (auto it = cache_.begin(); it != cache_.end(); ) {
            if (it->second.tableName == tableName)
                it = cache_.erase(it);
            else
                ++it;
        }
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        writeGen_.fetch_add(1, std::memory_order_acq_rel);
        cache_.clear();
    }

    // ── accessors ─────────────────────────────────────────────
    bool   isEnabled()  const { return enabled_; }
    void   setEnabled(bool v) { enabled_ = v; }
    size_t size()       const { std::lock_guard<std::mutex> lock(mu_); return cache_.size(); }
    size_t maxSize()    const { return maxSize_; }
    size_t hits()       const { std::lock_guard<std::mutex> lock(mu_); return totalHits_; }
    size_t misses()     const { std::lock_guard<std::mutex> lock(mu_); return totalMisses_; }
    int    ttl()        const { return ttlSeconds_; }

    // ── showStats ─────────────────────────────────────────────
    void showStats() const {
        std::lock_guard<std::mutex> lock(mu_);
        size_t total = totalHits_ + totalMisses_;
        std::string hitRate = total > 0
            ? std::to_string(totalHits_ * 100 / total) + "%"
            : "N/A";

        struct Row { std::string key, val; };
        std::vector<Row> rows = {
            {"Status",       enabled_ ? "ON" : "OFF"},
            {"Eintraege",    std::to_string(cache_.size()) + " / " + std::to_string(maxSize_)},
            {"TTL",          std::to_string(ttlSeconds_) + "s"},
            {"Cache Hits",   std::to_string(totalHits_)},
            {"Cache Misses", std::to_string(totalMisses_)},
            {"Hit-Rate",     hitRate},
        };

        size_t kw = 0, vw = 0;
        for (const auto& r : rows) {
            kw = std::max(kw, r.key.size());
            vw = std::max(vw, r.val.size());
        }
        // Prepare entry preview rows
        struct EntRow { std::string sql, val; };
        std::vector<EntRow> entries;
        for (const auto& [sql, entry] : cache_) {
            std::string short_sql = sql.size() > 40 ? sql.substr(0, 37) + "..." : sql;
            std::string hits_s = std::to_string(entry.hitCount) + " Hits";
            kw = std::max(kw, short_sql.size() + 2);
            vw = std::max(vw, hits_s.size());
            entries.push_back({short_sql, hits_s});
        }

        size_t boxW = kw + vw + 7;
        std::string title = "Query Cache \u2014 Statistiken";
        auto pad_to = [](const std::string& s, size_t w) {
            std::string r = s;
            while (r.size() < w) r += ' ';
            return r;
        };
        auto hline = [&](const std::string& l, const std::string& r) {
            std::cout << "  " << l;
            for (size_t i = 0; i < boxW; ++i) std::cout << "\u2500";
            std::cout << r << "\n";
        };
        size_t pad = (boxW > title.size() + 2) ? (boxW - title.size()) / 2 : 1;
        std::cout << "\n";
        hline("\u250c", "\u2510");
        std::cout << "  \u2502";
        for (size_t i = 0; i < pad; ++i) std::cout << " ";
        std::cout << title;
        size_t after = boxW - pad - title.size();
        for (size_t i = 0; i < after; ++i) std::cout << " ";
        std::cout << "\u2502\n";
        hline("\u251c", "\u2524");
        for (const auto& row : rows) {
            std::cout << "  \u2502  " << pad_to(row.key, kw)
                      << " : " << pad_to(row.val, vw) << "  \u2502\n";
        }
        if (!entries.empty()) {
            hline("\u251c", "\u2524");
            std::string hdr = "  Gecachte Queries";
            std::cout << "  \u2502" << hdr;
            for (size_t i = hdr.size(); i < boxW; ++i) std::cout << " ";
            std::cout << "\u2502\n";
            for (const auto& e : entries) {
                std::cout << "  \u2502  " << pad_to("  " + e.sql, kw)
                          << " : " << pad_to(e.val, vw) << "  \u2502\n";
            }
        }
        hline("\u2514", "\u2518");
        std::cout << "\n";
    }

private:
    mutable std::mutex mu_;
    std::map<std::string, CacheEntry> cache_;
    size_t maxSize_    = 100;
    int    ttlSeconds_ = 30;
    bool   enabled_    = true;
    size_t totalHits_  = 0;
    size_t totalMisses_ = 0;
    std::atomic<uint64_t> writeGen_{0};
};

} // namespace milansql
