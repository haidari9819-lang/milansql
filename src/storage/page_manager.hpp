#pragma once
// ============================================================
// page_manager.hpp — Phase 84: LRU Page Cache + File I/O
// Included inside namespace milansql in engine.hpp (after Page).
// Do NOT add a namespace milansql wrapper here.
//
// Responsibilities:
//   - Map tableName → list of pages on disk (.mdb files)
//   - LRU cache: keep up to DEFAULT_MAX_PAGES pages in memory
//   - readPage / writePage / allocatePage / flushDirtyPages
//   - showStats: cache hit ratio, dirty page count, total pages
// ============================================================

struct PageManager {
    static constexpr size_t DEFAULT_MAX_PAGES = 1000;

    // ── Cache entry ───────────────────────────────────────────
    struct CacheEntry {
        Page     page;
        bool     dirty      = false;
        uint64_t accessTime = 0;   // logical clock
    };

    // cache_[tableName][pageId] = entry
    std::unordered_map<std::string,
        std::unordered_map<uint64_t, CacheEntry>> cache_;

    size_t   maxPages_   = DEFAULT_MAX_PAGES;
    uint64_t clock_      = 0;
    size_t   cacheHits_  = 0;
    size_t   cacheMisses_= 0;

    // ── File path for a table ─────────────────────────────────
    static std::string pageFilePath(const std::string& tableName) {
        // Prevent path traversal via malicious table names
        for (char c : tableName) {
            if (c == '/' || c == '\\' || c == '\0') {
                throw std::runtime_error("Invalid table name: contains path separator");
            }
        }
        if (tableName.find("..") != std::string::npos) {
            throw std::runtime_error("Invalid table name: contains path traversal");
        }
        return tableName + ".mdb";
    }

    // ── Total cached pages across all tables ──────────────────
    size_t totalCached() const {
        size_t n = 0;
        for (const auto& [t, m] : cache_) n += m.size();
        return n;
    }

    // ── Evict the LRU dirty/clean page from any table ─────────
    void evictOne() {
        std::string   bestTable;
        uint64_t      bestPage = 0;
        uint64_t      bestTime = UINT64_MAX;
        bool          found    = false;

        for (const auto& [t, m] : cache_) {
            for (const auto& [pid, entry] : m) {
                if (entry.accessTime < bestTime) {
                    bestTime  = entry.accessTime;
                    bestTable = t;
                    bestPage  = pid;
                    found     = true;
                }
            }
        }
        if (!found) return;

        auto& entry = cache_[bestTable][bestPage];
        if (entry.dirty) {
            // flush this page to disk before evicting
            std::string path = pageFilePath(bestTable);
            std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
            if (f.is_open()) {
                f.seekp(static_cast<std::streamoff>(bestPage) *
                        static_cast<std::streamoff>(Page::PAGE_SIZE));
                f.write(reinterpret_cast<const char*>(entry.page.raw_), Page::PAGE_SIZE);
            }
        }
        cache_[bestTable].erase(bestPage);
        if (cache_[bestTable].empty()) cache_.erase(bestTable);
    }

    // ── Ensure cache has room for one more entry ──────────────
    void makeRoom() {
        while (totalCached() >= maxPages_) evictOne();
    }

    // ── Count pages on disk for a table ───────────────────────
    uint64_t getPageCount(const std::string& tableName) const {
        std::string path = pageFilePath(tableName);
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f.is_open()) return 0;
        auto sz = f.tellg();
        if (sz <= 0) return 0;
        return static_cast<uint64_t>(sz) / Page::PAGE_SIZE;
    }

    // ── Read a page (cache-first) ─────────────────────────────
    Page readPage(const std::string& tableName, uint64_t pageId) {
        auto& tblCache = cache_[tableName];
        auto it = tblCache.find(pageId);
        if (it != tblCache.end()) {
            ++cacheHits_;
            it->second.accessTime = ++clock_;
            return it->second.page;
        }
        ++cacheMisses_;

        // Read from disk
        Page pg;
        std::string path = pageFilePath(tableName);
        std::ifstream f(path, std::ios::binary);
        if (f.is_open()) {
            f.seekg(static_cast<std::streamoff>(pageId) *
                    static_cast<std::streamoff>(Page::PAGE_SIZE));
            f.read(reinterpret_cast<char*>(pg.raw_), Page::PAGE_SIZE);
        } else {
            pg.init(pageId, tableName);
        }

        makeRoom();
        CacheEntry& ce  = tblCache[pageId];
        ce.page        = pg;
        ce.dirty       = false;
        ce.accessTime  = ++clock_;
        return pg;
    }

    // ── Write (update) a cached page ─────────────────────────
    void writePage(const std::string& tableName, const Page& pg) {
        makeRoom();
        auto& ce       = cache_[tableName][pg.pageId()];
        ce.page        = pg;
        ce.dirty       = true;
        ce.accessTime  = ++clock_;
    }

    // ── Allocate a new blank page (appended to file) ──────────
    // Returns the new page already in cache (dirty).
    Page allocatePage(const std::string& tableName) {
        uint64_t newId = getPageCount(tableName);
        Page pg;
        pg.init(newId, tableName);

        // Create/extend the file with a zeroed page slot
        std::string path = pageFilePath(tableName);
        {
            std::ofstream f(path,
                std::ios::binary | std::ios::app);
            // Write an empty page as a placeholder so the file grows
            uint8_t zeros[Page::PAGE_SIZE] = {};
            f.write(reinterpret_cast<const char*>(zeros), Page::PAGE_SIZE);
        }

        // Put in cache (dirty so it will be flushed with actual data)
        makeRoom();
        auto& ce      = cache_[tableName][newId];
        ce.page       = pg;
        ce.dirty      = true;
        ce.accessTime = ++clock_;
        return pg;
    }

    // ── Flush all dirty pages for a table to disk ─────────────
    void flushDirtyPages(const std::string& tableName) {
        auto it = cache_.find(tableName);
        if (it == cache_.end()) return;

        std::string path = pageFilePath(tableName);
        std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
        if (!f.is_open()) {
            // Create the file
            f.open(path, std::ios::out | std::ios::binary);
            f.close();
            f.open(path, std::ios::in | std::ios::out | std::ios::binary);
        }

        for (auto& [pid, entry] : it->second) {
            if (!entry.dirty) continue;
            f.seekp(static_cast<std::streamoff>(pid) *
                    static_cast<std::streamoff>(Page::PAGE_SIZE));
            f.write(reinterpret_cast<const char*>(entry.page.raw_), Page::PAGE_SIZE);
            entry.dirty = false;
        }
    }

    // ── Flush all dirty pages for all tables ──────────────────
    void flushAll() {
        for (auto& [tableName, _] : cache_)
            flushDirtyPages(tableName);
    }

    // ── Statistics string ─────────────────────────────────────
    std::string showStats() const {
        size_t dirty = 0;
        for (const auto& [t, m] : cache_)
            for (const auto& [pid, e] : m)
                if (e.dirty) ++dirty;

        size_t total = cacheHits_ + cacheMisses_;
        double hitRatio = total ? (100.0 * cacheHits_ / total) : 0.0;

        char buf[512];
        std::snprintf(buf, sizeof(buf),
            "  Page Manager Stats\n"
            "  ------------------\n"
            "  Cached pages : %zu / %zu\n"
            "  Dirty pages  : %zu\n"
            "  Cache hits   : %zu\n"
            "  Cache misses : %zu\n"
            "  Hit ratio    : %.1f%%\n",
            totalCached(), maxPages_,
            dirty,
            cacheHits_, cacheMisses_,
            hitRatio);
        return std::string(buf);
    }
};
