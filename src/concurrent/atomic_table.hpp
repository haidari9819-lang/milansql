#pragma once
// ============================================================
// atomic_table.hpp — Phase 112: Lock-free Statistics Counters
//
// All counters use std::atomic<uint64_t> — no mutex needed.
// Supports concurrent increment from multiple threads.
//
// AtomicTable concept:
//   - selectAll()    → shared_lock  (many parallel readers)
//   - insertRow()    → unique_lock  (one exclusive writer)
//   - updateWhere()  → unique_lock
//   - deleteWhere()  → unique_lock
// ============================================================

#include <atomic>
#include <cstdint>
#include <string>

namespace milansql {

// ── Global lock-free statistics (Phase 112) ───────────────────
struct AtomicStats {
    // Query counters
    std::atomic<uint64_t> queryCount{0};           // total SQL queries dispatched
    std::atomic<uint64_t> readCount{0};            // SELECT operations (shared lock path)
    std::atomic<uint64_t> writeCount{0};           // INSERT/UPDATE/DELETE (exclusive lock path)

    // Cache counters
    std::atomic<uint64_t> hitCount{0};             // query cache hits
    std::atomic<uint64_t> missCount{0};            // query cache misses

    // RwLock counters
    std::atomic<uint64_t> rwlockReadAcquires{0};   // successful read lock acquisitions
    std::atomic<uint64_t> rwlockWriteAcquires{0};  // successful write lock acquisitions
    std::atomic<uint64_t> rwlockSkipped{0};        // locks skipped (recursion guard)

    // Helpers
    void incRead()  { readCount.fetch_add(1,  std::memory_order_relaxed);
                      rwlockReadAcquires.fetch_add(1, std::memory_order_relaxed); }
    void incWrite() { writeCount.fetch_add(1, std::memory_order_relaxed);
                      rwlockWriteAcquires.fetch_add(1, std::memory_order_relaxed); }
    void incSkip()  { rwlockSkipped.fetch_add(1, std::memory_order_relaxed); }
    void incQuery() { queryCount.fetch_add(1, std::memory_order_relaxed); }
    void incHit()   { hitCount.fetch_add(1,   std::memory_order_relaxed); }
    void incMiss()  { missCount.fetch_add(1,  std::memory_order_relaxed); }
};

// Singleton accessor
inline AtomicStats& g_atomicStats() {
    static AtomicStats s;
    return s;
}

} // namespace milansql
