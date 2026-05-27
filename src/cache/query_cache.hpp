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

    // ── get ───────────────────────────────────────────────────
    std::optional<std::string> get(const std::string& sql) {
        if (!enabled_) return std::nullopt;
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

    // ── put ───────────────────────────────────────────────────
    void put(const std::string& sql, const std::string& output,
             const std::string& tableName) {
        if (!enabled_) return;

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
        for (auto it = cache_.begin(); it != cache_.end(); ) {
            if (it->second.tableName == tableName)
                it = cache_.erase(it);
            else
                ++it;
        }
    }

    void clear() { cache_.clear(); }

    // ── accessors ─────────────────────────────────────────────
    bool   isEnabled()  const { return enabled_; }
    void   setEnabled(bool v) { enabled_ = v; }
    size_t size()       const { return cache_.size(); }
    size_t maxSize()    const { return maxSize_; }
    size_t hits()       const { return totalHits_; }
    size_t misses()     const { return totalMisses_; }
    int    ttl()        const { return ttlSeconds_; }

    // ── showStats ─────────────────────────────────────────────
    void showStats() const {
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
    std::map<std::string, CacheEntry> cache_;
    size_t maxSize_    = 100;
    int    ttlSeconds_ = 30;
    bool   enabled_    = true;
    size_t totalHits_  = 0;
    size_t totalMisses_ = 0;
};

} // namespace milansql
