#pragma once

#include <string>
#include <map>
#include <iostream>
#include <ctime>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <functional>
#include <cstdint>
#include <sstream>
#include "../utils/date_utils.hpp"

// ============================================================
// vacuum.hpp — Phase 85: Auto-Vacuum Manager
//              Phase 171: MVCC GC with minActiveTxId horizon
//
// Tracks dead tuples per table (rows with xmax != 0).
//
// Auto-vacuum background thread:
//   - Runs every 60 seconds (AUTO_VACUUM_INTERVAL)
//   - Calls the vacuumAll callback, which only removes row
//     versions no active transaction can still see
//     (xmax committed AND xmax < minActiveTxId)
//
// Run statistics (freed rows/bytes, last run) are exposed via
// statsJson() for the HTTP endpoint /vacuum/stats.
//
// Note: included from engine.hpp BEFORE namespace milansql {}
// with its own namespace wrapper.
// ============================================================

namespace milansql {

class VacuumManager {
public:
    static constexpr size_t DEFAULT_THRESHOLD    = 100;
    static constexpr int    AUTO_VACUUM_INTERVAL = 60;   // seconds (Phase 171)

    VacuumManager()  = default;
    ~VacuumManager() { stopAutoVacuum(); }

    // ── Dead tuple tracking ───────────────────────────────────

    // Increment dead tuple counter for a table (call after DELETE/UPDATE)
    void addDeadTuples(const std::string& tbl, size_t count = 1) {
        std::lock_guard<std::mutex> lk(dataMu_);
        deadTupleCount_[tbl] += count;
    }

    // Reset dead tuple counter (after successful vacuum)
    void resetDeadTuples(const std::string& tbl) {
        std::lock_guard<std::mutex> lk(dataMu_);
        deadTupleCount_[tbl] = 0;
        lastVacuumTime_[tbl] = currentTime();
    }

    void resetAllDeadTuples(const std::string& tbl) {
        resetDeadTuples(tbl);
    }

    // Should we auto-vacuum this table?
    bool shouldAutoVacuum(const std::string& tbl) const {
        std::lock_guard<std::mutex> lk(dataMu_);
        if (!autoVacuumEnabled_) return false;
        auto it = deadTupleCount_.find(tbl);
        if (it == deadTupleCount_.end()) return false;
        return it->second >= autoVacuumThreshold_;
    }

    bool anyTableNeedsVacuum() const {
        std::lock_guard<std::mutex> lk(dataMu_);
        if (!autoVacuumEnabled_) return false;
        for (const auto& [t, n] : deadTupleCount_)
            if (n >= autoVacuumThreshold_) return true;
        return false;
    }

    size_t getDeadTupleCount(const std::string& tbl) const {
        std::lock_guard<std::mutex> lk(dataMu_);
        auto it = deadTupleCount_.find(tbl);
        return it == deadTupleCount_.end() ? 0 : it->second;
    }

    size_t getTotalDeadTuples() const {
        std::lock_guard<std::mutex> lk(dataMu_);
        size_t total = 0;
        for (const auto& [t, n] : deadTupleCount_) total += n;
        return total;
    }

    // ── Phase 171: vacuum run statistics ──────────────────────

    // Called after every vacuum pass (manual VACUUM or auto-vacuum).
    void recordVacuumRun(size_t freedRows, size_t freedBytes, bool automatic) {
        std::lock_guard<std::mutex> lk(dataMu_);
        totalFreedRows_  += freedRows;
        totalFreedBytes_ += freedBytes;
        lastFreedRows_    = freedRows;
        lastFreedBytes_   = freedBytes;
        lastRunUnix_      = std::time(nullptr);
        lastRunTime_      = currentTime();
        ++vacuumRuns_;
        if (automatic) {
            ++autoVacuumCount_;
            lastAutoVacuumTime_ = lastRunTime_;
        }
    }

    size_t getTotalFreedRows()  const { std::lock_guard<std::mutex> lk(dataMu_); return totalFreedRows_; }
    size_t getTotalFreedBytes() const { std::lock_guard<std::mutex> lk(dataMu_); return totalFreedBytes_; }
    size_t getVacuumRuns()      const { std::lock_guard<std::mutex> lk(dataMu_); return vacuumRuns_; }
    std::string getLastRunTime() const { std::lock_guard<std::mutex> lk(dataMu_); return lastRunTime_; }

    // JSON for HTTP endpoint /vacuum/stats
    std::string statsJson() const {
        std::lock_guard<std::mutex> lk(dataMu_);
        long long secsSince = -1;
        if (lastRunUnix_ != 0)
            secsSince = (long long)(std::time(nullptr) - lastRunUnix_);
        // Phase 173: seconds until next scheduled auto-vacuum run
        long long nextRun = AUTO_VACUUM_INTERVAL;
        if (secsSince >= 0) {
            nextRun = AUTO_VACUUM_INTERVAL - secsSince;
            if (nextRun < 0) nextRun = 0;
        }
        size_t pending = 0;
        for (const auto& [t, n] : deadTupleCount_) pending += n;
        std::ostringstream oss;
        oss << "{"
            << "\"auto_vacuum_enabled\":" << (autoVacuumEnabled_ ? "true" : "false") << ","
            << "\"interval_seconds\":"    << AUTO_VACUUM_INTERVAL << ","
            << "\"threshold\":"           << autoVacuumThreshold_ << ","
            << "\"runs_total\":"          << vacuumRuns_ << ","
            << "\"auto_runs\":"           << autoVacuumCount_ << ","
            << "\"last_run\":\""          << (lastRunTime_.empty() ? "never" : lastRunTime_) << "\","
            << "\"seconds_since_last_run\":" << secsSince << ","
            << "\"next_auto_run_in_seconds\":" << nextRun << ","
            << "\"last_freed_rows\":"     << lastFreedRows_ << ","
            << "\"last_freed_bytes\":"    << lastFreedBytes_ << ","
            << "\"total_freed_rows\":"    << totalFreedRows_ << ","
            << "\"total_freed_bytes\":"   << totalFreedBytes_ << ","
            << "\"pending_dead_tuples\":" << pending << ","
            << "\"tables\":{";
        bool first = true;
        for (const auto& [tbl, cnt] : deadTupleCount_) {
            if (!first) oss << ",";
            first = false;
            oss << "\"" << tbl << "\":" << cnt;
        }
        oss << "},\"last_vacuum_per_table\":{";
        first = true;
        for (const auto& [tbl, ts] : lastVacuumTime_) {
            if (!first) oss << ",";
            first = false;
            oss << "\"" << tbl << "\":\"" << ts << "\"";
        }
        oss << "}}";
        return oss.str();
    }

    // ── Settings ──────────────────────────────────────────────
    void setAutoVacuumEnabled(bool on) { autoVacuumEnabled_ = on; }
    bool isAutoVacuumEnabled() const   { return autoVacuumEnabled_; }

    void setAutoVacuumThreshold(size_t n) { autoVacuumThreshold_ = n; }
    size_t getAutoVacuumThreshold() const { return autoVacuumThreshold_; }

    // ── Auto-vacuum background thread (Phase 171) ────────────
    // vacuumAllFn: callback that performs the actual vacuum on all tables
    //              and returns total rows cleaned.
    // Runs every intervalSec seconds (default 60) whenever auto-vacuum
    // is enabled — the callback itself only removes versions that are
    // invisible to every active transaction (minActiveTxId horizon).
    void startAutoVacuum(std::function<size_t()> vacuumAllFn,
                         int intervalSec = AUTO_VACUUM_INTERVAL) {
        if (autoVacuumRunning_) return;
        autoVacuumRunning_ = true;
        autoVacuumThread_ = std::thread([this, fn = std::move(vacuumAllFn), intervalSec]() {
            while (autoVacuumRunning_) {
                // Sleep in small increments so we can stop quickly
                for (int i = 0; i < intervalSec * 10 && autoVacuumRunning_; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));

                if (!autoVacuumRunning_) break;
                if (!isAutoVacuumEnabled()) continue;
                size_t cleaned = fn();
                if (cleaned > 0) {
                    std::lock_guard<std::mutex> lk(dataMu_);
                    lastAutoVacuumTime_ = currentTime();
                    // Reset all counters (vacuumAll cleaned everything)
                    for (auto& [t, n] : deadTupleCount_) {
                        lastVacuumTime_[t] = lastAutoVacuumTime_;
                        n = 0;
                    }
                }
            }
        });
    }

    void stopAutoVacuum() {
        autoVacuumRunning_ = false;
        if (autoVacuumThread_.joinable())
            autoVacuumThread_.join();
    }

    // ── Statistics display ────────────────────────────────────
    void showStatus() const {
        std::lock_guard<std::mutex> lk(dataMu_);
        std::cout << "\n  Auto-Vacuum Status\n";
        std::cout << "  ─────────────────────────────────────────\n";
        std::cout << "  Auto-Vacuum        : " << (autoVacuumEnabled_ ? "ON" : "OFF") << "\n";
        std::cout << "  Threshold          : " << autoVacuumThreshold_ << " Dead Tuples\n";
        std::cout << "  Auto-Vacuum-Laeufe : " << autoVacuumCount_ << "\n";
        std::cout << "  Letzter Auto-Vacuum: " << (lastAutoVacuumTime_.empty() ? "(nie)" : lastAutoVacuumTime_) << "\n";
        size_t totalDead = 0;
        for (const auto& [t, n] : deadTupleCount_) totalDead += n;
        std::cout << "  Gesamt Dead Tuples : " << totalDead << "\n";

        if (!deadTupleCount_.empty()) {
            std::cout << "\n  Dead Tuples per Tabelle:\n";
            for (const auto& [tbl, cnt] : deadTupleCount_) {
                auto lt = lastVacuumTime_.find(tbl);
                std::string vacTime = (lt != lastVacuumTime_.end() && !lt->second.empty())
                    ? lt->second : "(nie)";
                std::cout << "    " << tbl << ": " << cnt
                          << " dead tuples  (letzter Vacuum: " << vacTime << ")\n";
            }
        }
        std::cout << "\n";
    }

private:
    mutable std::mutex                 dataMu_;  // Bug #23: protects maps below
    std::map<std::string, size_t>      deadTupleCount_;
    std::map<std::string, std::string> lastVacuumTime_;

    bool   autoVacuumEnabled_   = true;
    size_t autoVacuumThreshold_ = DEFAULT_THRESHOLD;
    size_t autoVacuumCount_     = 0;
    std::string lastAutoVacuumTime_;

    // Phase 171: run statistics
    size_t      vacuumRuns_      = 0;
    size_t      totalFreedRows_  = 0;
    size_t      totalFreedBytes_ = 0;
    size_t      lastFreedRows_   = 0;
    size_t      lastFreedBytes_  = 0;
    std::string lastRunTime_;
    std::time_t lastRunUnix_     = 0;

    std::atomic<bool> autoVacuumRunning_{false};
    std::thread       autoVacuumThread_;

    static std::string currentTime() {
        std::time_t t = std::time(nullptr);
        char buf[20] = {};
        std::tm ltm = milansql::safe_localtime(&t);
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ltm);
        return buf;
    }
};

} // namespace milansql
