#pragma once
// ============================================================
// rwlock.hpp — Phase 112: Reader-Writer Lock
//
// Wraps std::shared_mutex for per-table concurrent access:
//   - Multiple readers simultaneously (shared lock)
//   - Only one writer (exclusive lock)
//   - Readers never block each other
//
// Reference: C++17 std::shared_mutex
// ============================================================

#include <shared_mutex>
#include <atomic>
#include <cstdint>

namespace milansql {

class RwLock {
public:
    // ── Primitive lock/unlock ──────────────────────────────────
    void readLock()    { mu_.lock_shared();   activeReaders_.fetch_add(1, std::memory_order_relaxed); }
    void readUnlock()  { activeReaders_.fetch_sub(1, std::memory_order_relaxed); mu_.unlock_shared(); }
    void writeLock()   { mu_.lock();           activeWriter_.store(true,  std::memory_order_relaxed); }
    void writeUnlock() { activeWriter_.store(false, std::memory_order_relaxed); mu_.unlock(); }

    // ── Approximate stats (for SHOW ENGINE STATUS) ─────────────
    int  activeReaders()    const { return activeReaders_.load(std::memory_order_relaxed); }
    bool hasActiveWriter()  const { return activeWriter_.load(std::memory_order_relaxed); }

    // ── RAII guards ────────────────────────────────────────────
    struct ReadGuard {
        explicit ReadGuard(RwLock& lk) : lk_(&lk) { lk_->readLock(); }
        ~ReadGuard() { if (lk_) lk_->readUnlock(); }
        ReadGuard(ReadGuard&& o) noexcept : lk_(o.lk_) { o.lk_ = nullptr; }
        ReadGuard(const ReadGuard&)            = delete;
        ReadGuard& operator=(const ReadGuard&) = delete;
    private:
        RwLock* lk_;
    };

    struct WriteGuard {
        explicit WriteGuard(RwLock& lk) : lk_(&lk) { lk_->writeLock(); }
        ~WriteGuard() { if (lk_) lk_->writeUnlock(); }
        WriteGuard(WriteGuard&& o) noexcept : lk_(o.lk_) { o.lk_ = nullptr; }
        WriteGuard(const WriteGuard&)            = delete;
        WriteGuard& operator=(const WriteGuard&) = delete;
    private:
        RwLock* lk_;
    };

    // Non-copyable, non-movable (shared_mutex is neither)
    RwLock()                           = default;
    RwLock(const RwLock&)              = delete;
    RwLock& operator=(const RwLock&)   = delete;
    RwLock(RwLock&&)                   = delete;
    RwLock& operator=(RwLock&&)        = delete;

private:
    std::shared_mutex   mu_;
    std::atomic<int>    activeReaders_{0};
    std::atomic<bool>   activeWriter_{false};
};

} // namespace milansql
