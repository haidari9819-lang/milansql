#pragma once
// ============================================================
// lsn_manager.hpp — Phase 114: Log Sequence Number Manager
//
// Maintains a monotonically increasing Log Sequence Number (LSN)
// used to order WAL entries and coordinate recovery.
//
// LSN lifecycle:
//   nextLsn()      — allocate the next LSN for a WAL record
//   markFlushed()  — record that all entries up to this LSN
//                    have been written to stable storage
//   isFlushed()    — check if a given LSN is durable
//   waitForLsn()   — spin-wait until the LSN is durable
//                    (used by synchronous-commit callers)
//
// Persistence:
//   LSN state is persisted in "database.lsn" (text key=value).
//   Format:
//     currentLsn:<uint64>
//     lastFlushedLsn:<uint64>
//     timestamp:<YYYY-MM-DD HH:MM:SS>
//
// Reference: PostgreSQL pg_lsn, LSN-based recovery concepts
// ============================================================

#include <atomic>
#include <mutex>
#include <string>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdint>
#include <ctime>
#include <thread>
#include <chrono>

namespace milansql {

// ── LsnManager ────────────────────────────────────────────────
class LsnManager {
public:
    static constexpr uint64_t    INITIAL_LSN    = 1;
    static constexpr const char* LSN_FILE       = "database.lsn";
    static constexpr int         WAIT_TIMEOUT_MS = 1000;  // 1 s max wait

    explicit LsnManager(std::string lsnFile = LSN_FILE)
        : lsnFilePath_(std::move(lsnFile))
        , currentLsn_(INITIAL_LSN)
        , lastFlushedLsn_(0)
    {
        load();
    }

    // ── Allocate the next LSN ────────────────────────────────────
    // Returns the LSN to stamp on the WAL entry being written.
    uint64_t nextLsn() {
        return currentLsn_.fetch_add(1, std::memory_order_acq_rel);
    }

    // ── Peek at the current LSN without allocating ───────────────
    uint64_t currentLsn() const {
        return currentLsn_.load(std::memory_order_acquire);
    }

    // ── Mark everything up to (and including) lsn as flushed ────
    void markFlushed(uint64_t lsn) {
        std::lock_guard<std::mutex> g(mu_);
        if (lsn > lastFlushedLsn_) {
            lastFlushedLsn_ = lsn;
            persist();
        }
    }

    // ── Return the highest LSN confirmed on stable storage ───────
    uint64_t lastFlushedLsn() const {
        std::lock_guard<std::mutex> g(mu_);
        return lastFlushedLsn_;
    }

    // ── Non-blocking durability check ────────────────────────────
    bool isFlushed(uint64_t lsn) const {
        std::lock_guard<std::mutex> g(mu_);
        return lastFlushedLsn_ >= lsn;
    }

    // ── Blocking wait (up to WAIT_TIMEOUT_MS) ────────────────────
    // Returns true if the LSN became durable within the timeout.
    bool waitForLsn(uint64_t lsn) const {
        using namespace std::chrono;
        auto deadline = steady_clock::now() + milliseconds(WAIT_TIMEOUT_MS);
        while (steady_clock::now() < deadline) {
            if (isFlushed(lsn)) return true;
            std::this_thread::sleep_for(milliseconds(1));
        }
        return isFlushed(lsn);  // final check
    }

    // ── Persist LSN state to disk ────────────────────────────────
    void persist() {
        // Caller must hold mu_ OR be in a single-threaded startup context.
        std::ofstream f(lsnFilePath_);
        if (!f) return;
        f << "currentLsn:" << currentLsn_.load(std::memory_order_relaxed) << "\n";
        f << "lastFlushedLsn:" << lastFlushedLsn_ << "\n";

        // Timestamp
        std::time_t now = std::time(nullptr);
        char buf[32];
        struct tm tmBuf{};
#if defined(_WIN32)
        localtime_s(&tmBuf, &now);
#else
        localtime_r(&now, &tmBuf);
#endif
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmBuf);
        f << "timestamp:" << buf << "\n";
    }

    // ── Load LSN state from disk ─────────────────────────────────
    void load() {
        std::ifstream f(lsnFilePath_);
        if (!f) return;

        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.size() > 11 && line.substr(0, 11) == "currentLsn:") {
                try {
                    uint64_t v = std::stoull(line.substr(11));
                    currentLsn_.store(v, std::memory_order_relaxed);
                } catch (...) {}
            } else if (line.size() > 15 && line.substr(0, 15) == "lastFlushedLsn:") {
                try {
                    lastFlushedLsn_ = std::stoull(line.substr(15));
                } catch (...) {}
            }
        }
    }

    // ── Show status (for SHOW ENGINE STATUS / SHOW RECOVERY LOG) ─
    void showStatus() const {
        std::lock_guard<std::mutex> g(mu_);
        std::cout << "  LSN Manager:\n";
        std::cout << "  ├─ Current LSN      : "
                  << currentLsn_.load(std::memory_order_relaxed) << "\n";
        std::cout << "  └─ Last Flushed LSN : " << lastFlushedLsn_ << "\n";
    }

private:
    std::string           lsnFilePath_;
    std::atomic<uint64_t> currentLsn_;
    uint64_t              lastFlushedLsn_;
    mutable std::mutex    mu_;
};

// ── Global singleton ───────────────────────────────────────────
inline LsnManager& g_lsnManager() {
    static LsnManager mgr(LsnManager::LSN_FILE);
    return mgr;
}

} // namespace milansql
