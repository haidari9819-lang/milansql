#pragma once

#include <string>
#include <map>
#include <vector>
#include <chrono>
#include <functional>
#include <atomic>
#include <iostream>
#include <iomanip>
#include <sstream>

// ============================================================
// buffer_pool.hpp — Phase 73: Buffer Pool Manager
//
// Simulates InnoDB-style Buffer Pool for MilanSQL:
//   - Tracks which tables are "hot" (in pool)
//   - LRU eviction when pool is full
//   - Dirty page tracking (modified since last flush)
//   - Hit/miss counters
//   - Write-behind: dirty pages flushed periodically or on FLUSH
//
// Each "page" = one table (since MilanSQL is in-memory).
// Pool size in MB determines maxPages_ (1 page ≈ 1 MB budget).
// ============================================================

namespace milansql {

// ── Assumed page size for capacity calculation ──────────────
static constexpr int BUFFER_PAGE_SIZE_MB = 1;  // 1 page = 1 MB

class BufferPool {
public:
    // Metadata for one "page" (table) in the pool
    struct PageMeta {
        bool dirty     = false;
        int  pinCount  = 0;
        std::chrono::steady_clock::time_point lastAccess
            = std::chrono::steady_clock::now();
    };

    // Snapshot of pool statistics for SHOW BUFFER POOL STATUS
    struct Status {
        int       sizeMB        = 128;
        int       usedPages     = 0;
        int       dirtyPages    = 0;
        double    hitRate       = 0.0;
        long long totalRequests = 0;
        long long cacheHits     = 0;
        long long cacheMisses   = 0;
        long long pagesEvicted  = 0;
    };

    explicit BufferPool(int sizeMB = 128)
        : sizeMB_(sizeMB)
        , maxPages_(std::max(1, sizeMB / BUFFER_PAGE_SIZE_MB))
    {}

    // ── Pool size control ──────────────────────────────────────
    void setSizeMB(int mb) {
        if (mb < 1) mb = 1;
        sizeMB_  = mb;
        maxPages_ = std::max(1, mb / BUFFER_PAGE_SIZE_MB);
    }
    int getSizeMB() const { return sizeMB_; }

    // ── Access tracking ────────────────────────────────────────
    // Call on every table read. Returns true = hit, false = miss.
    bool access(const std::string& tableName) {
        ++totalRequests_;
        auto it = pages_.find(tableName);
        if (it != pages_.end()) {
            // Cache hit — update LRU timestamp
            it->second.lastAccess = std::chrono::steady_clock::now();
            ++hits_;
            return true;
        }
        // Cache miss — load into pool
        ++misses_;
        loadPage(tableName);
        return false;
    }

    // ── Dirty tracking ─────────────────────────────────────────
    void markDirty(const std::string& tableName) {
        auto it = pages_.find(tableName);
        if (it != pages_.end()) {
            it->second.dirty = true;
            it->second.lastAccess = std::chrono::steady_clock::now();
        } else {
            loadPage(tableName);
            pages_[tableName].dirty = true;
        }
    }

    void markClean(const std::string& tableName) {
        auto it = pages_.find(tableName);
        if (it != pages_.end()) it->second.dirty = false;
    }

    // Returns names of all dirty tables
    std::vector<std::string> getDirtyPages() const {
        std::vector<std::string> result;
        for (const auto& [name, meta] : pages_)
            if (meta.dirty) result.push_back(name);
        return result;
    }

    // ── Pinning ────────────────────────────────────────────────
    void pin(const std::string& tableName) {
        auto it = pages_.find(tableName);
        if (it != pages_.end()) ++it->second.pinCount;
    }
    void unpin(const std::string& tableName) {
        auto it = pages_.find(tableName);
        if (it != pages_.end() && it->second.pinCount > 0)
            --it->second.pinCount;
    }

    // ── Remove a page (e.g. after DROP TABLE) ─────────────────
    void removePage(const std::string& tableName) {
        pages_.erase(tableName);
    }

    // ── Flush all dirty pages via provided callback ───────────
    // flushFn(tableName) → caller saves that table to disk
    void flushAll(const std::function<void(const std::string&)>& flushFn) {
        for (auto& [name, meta] : pages_) {
            if (meta.dirty) {
                try { flushFn(name); } catch (...) {}
                meta.dirty = false;
            }
        }
    }

    // ── LRU eviction ──────────────────────────────────────────
    // Evicts oldest unpinned non-dirty page (or oldest dirty if all are dirty)
    void evictLRU() {
        if (pages_.empty()) return;

        // Find oldest unpinned clean page first
        std::string victim;
        std::chrono::steady_clock::time_point oldest
            = std::chrono::steady_clock::now();

        for (const auto& [name, meta] : pages_) {
            if (meta.pinCount == 0 && !meta.dirty &&
                meta.lastAccess <= oldest) {
                oldest  = meta.lastAccess;
                victim  = name;
            }
        }

        // If no clean candidate, evict oldest unpinned dirty page
        if (victim.empty()) {
            oldest = std::chrono::steady_clock::now();
            for (const auto& [name, meta] : pages_) {
                if (meta.pinCount == 0 && meta.lastAccess <= oldest) {
                    oldest = meta.lastAccess;
                    victim = name;
                }
            }
        }

        if (!victim.empty()) {
            pages_.erase(victim);
            ++evictions_;
        }
    }

    // ── Show buffer pool status (ASCII table) ─────────────────
    void showStatus() const {
        auto st = getStatus();
        std::string poolStr = std::to_string(st.sizeMB) + " MB";
        std::string hitStr  = [&]{
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(1) << st.hitRate << "%";
            return oss.str();
        }();

        auto row = [](const std::string& metric, const std::string& val) {
            int mw = 21, vw = 9;
            std::string m = metric, v = val;
            while ((int)m.size() < mw) m += ' ';
            while ((int)v.size() < vw) v = ' ' + v;
            std::cout << "  \u2502 " << m << " \u2502 " << v << " \u2502\n";
        };

        std::cout << "  \u250c\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u252c\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2510\n";
        std::cout << "  \u2502 Metric                \u2502 Value     \u2502\n";
        std::cout << "  \u251c\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u253c\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2524\n";
        row("Pool Size",       poolStr);
        row("Used Pages",      std::to_string(st.usedPages));
        row("Dirty Pages",     std::to_string(st.dirtyPages));
        row("Hit Rate",        hitStr);
        row("Total Requests",  std::to_string(st.totalRequests));
        row("Cache Hits",      std::to_string(st.cacheHits));
        row("Cache Misses",    std::to_string(st.cacheMisses));
        row("Pages Evicted",   std::to_string(st.pagesEvicted));
        std::cout << "  \u2514\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2534\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2518\n\n";
    }

    // ── Statistics ─────────────────────────────────────────────
    Status getStatus() const {
        Status st;
        st.sizeMB        = sizeMB_;
        st.usedPages     = static_cast<int>(pages_.size());
        st.dirtyPages    = 0;
        for (const auto& [n, m] : pages_)
            if (m.dirty) ++st.dirtyPages;
        st.totalRequests = totalRequests_.load();
        st.cacheHits     = hits_.load();
        st.cacheMisses   = misses_.load();
        st.pagesEvicted  = evictions_;
        if (st.totalRequests > 0)
            st.hitRate = 100.0 * st.cacheHits / st.totalRequests;
        return st;
    }

private:
    int sizeMB_;
    int maxPages_;
    std::map<std::string, PageMeta> pages_;
    std::atomic<long long> totalRequests_{0};
    std::atomic<long long> hits_{0};
    std::atomic<long long> misses_{0};
    long long evictions_ = 0;

    // Load a table into the pool, evicting LRU if full
    void loadPage(const std::string& tableName) {
        if ((int)pages_.size() >= maxPages_)
            evictLRU();
        pages_[tableName] = PageMeta{};
    }
};

} // namespace milansql
