#pragma once

#include <string>
#include <map>
#include <iostream>
#include <ctime>
#include <atomic>
#include <thread>
#include <chrono>
#include <functional>
#include <cstdint>

// ============================================================
// vacuum.hpp — Phase 85: Auto-Vacuum Manager
//
// Tracks dead tuples per table (rows with xmax != 0).
// Triggers VACUUM when deadTupleCount >= autoVacuumThreshold_.
//
// Auto-vacuum background thread:
//   - Checks every 30 seconds
//   - Calls vacuumAll callback if any table exceeds threshold
//
// Note: included from engine.hpp BEFORE namespace milansql {}
// with its own namespace wrapper.
// ============================================================

namespace milansql {

class VacuumManager {
public:
    static constexpr size_t DEFAULT_THRESHOLD    = 100;
    static constexpr int    AUTO_VACUUM_INTERVAL = 30;   // seconds

    VacuumManager()  = default;
    ~VacuumManager() { stopAutoVacuum(); }

    // ── Dead tuple tracking ───────────────────────────────────

    // Increment dead tuple counter for a table (call after DELETE/UPDATE)
    void addDeadTuples(const std::string& tbl, size_t count = 1) {
        deadTupleCount_[tbl] += count;
    }

    // Reset dead tuple counter (after successful vacuum)
    void resetDeadTuples(const std::string& tbl) {
        deadTupleCount_[tbl] = 0;
        lastVacuumTime_[tbl] = currentTime();
    }

    void resetAllDeadTuples(const std::string& tbl) {
        resetDeadTuples(tbl);
    }

    // Should we auto-vacuum this table?
    bool shouldAutoVacuum(const std::string& tbl) const {
        if (!autoVacuumEnabled_) return false;
        auto it = deadTupleCount_.find(tbl);
        if (it == deadTupleCount_.end()) return false;
        return it->second >= autoVacuumThreshold_;
    }

    bool anyTableNeedsVacuum() const {
        if (!autoVacuumEnabled_) return false;
        for (const auto& [t, n] : deadTupleCount_)
            if (n >= autoVacuumThreshold_) return true;
        return false;
    }

    size_t getDeadTupleCount(const std::string& tbl) const {
        auto it = deadTupleCount_.find(tbl);
        return it == deadTupleCount_.end() ? 0 : it->second;
    }

    size_t getTotalDeadTuples() const {
        size_t total = 0;
        for (const auto& [t, n] : deadTupleCount_) total += n;
        return total;
    }

    // ── Settings ──────────────────────────────────────────────
    void setAutoVacuumEnabled(bool on) { autoVacuumEnabled_ = on; }
    bool isAutoVacuumEnabled() const   { return autoVacuumEnabled_; }

    void setAutoVacuumThreshold(size_t n) { autoVacuumThreshold_ = n; }
    size_t getAutoVacuumThreshold() const { return autoVacuumThreshold_; }

    // ── Auto-vacuum background thread ────────────────────────
    // vacuumAllFn: callback that performs the actual vacuum on all tables
    //              and returns total rows cleaned
    void startAutoVacuum(std::function<size_t()> vacuumAllFn) {
        if (autoVacuumRunning_) return;
        autoVacuumRunning_ = true;
        autoVacuumThread_ = std::thread([this, fn = std::move(vacuumAllFn)]() {
            while (autoVacuumRunning_) {
                // Sleep in small increments so we can stop quickly
                for (int i = 0; i < AUTO_VACUUM_INTERVAL * 10 && autoVacuumRunning_; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));

                if (!autoVacuumRunning_) break;
                if (anyTableNeedsVacuum()) {
                    size_t cleaned = fn();
                    if (cleaned > 0) {
                        lastAutoVacuumTime_ = currentTime();
                        ++autoVacuumCount_;
                        // Reset all counters (vacuumAll cleaned everything)
                        for (auto& [t, n] : deadTupleCount_) {
                            lastVacuumTime_[t] = lastAutoVacuumTime_;
                            n = 0;
                        }
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
        std::cout << "\n  Auto-Vacuum Status\n";
        std::cout << "  ─────────────────────────────────────────\n";
        std::cout << "  Auto-Vacuum        : " << (autoVacuumEnabled_ ? "ON" : "OFF") << "\n";
        std::cout << "  Threshold          : " << autoVacuumThreshold_ << " Dead Tuples\n";
        std::cout << "  Auto-Vacuum-Laeufe : " << autoVacuumCount_ << "\n";
        std::cout << "  Letzter Auto-Vacuum: " << (lastAutoVacuumTime_.empty() ? "(nie)" : lastAutoVacuumTime_) << "\n";
        std::cout << "  Gesamt Dead Tuples : " << getTotalDeadTuples() << "\n";

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
    std::map<std::string, size_t>      deadTupleCount_;
    std::map<std::string, std::string> lastVacuumTime_;

    bool   autoVacuumEnabled_   = true;
    size_t autoVacuumThreshold_ = DEFAULT_THRESHOLD;
    size_t autoVacuumCount_     = 0;
    std::string lastAutoVacuumTime_;

    std::atomic<bool> autoVacuumRunning_{false};
    std::thread       autoVacuumThread_;

    static std::string currentTime() {
        std::time_t t = std::time(nullptr);
        char buf[20] = {};
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        return buf;
    }
};

} // namespace milansql
