#pragma once
// ============================================================
// event_scheduler.hpp — Event Scheduler für MilanSQL
// Phase 61: CREATE EVENT / ON SCHEDULE / DO
// ============================================================

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include "../utils/date_utils.hpp"

namespace milansql {

// ── EventDef ─────────────────────────────────────────────────
struct EventDef {
    std::string name;
    bool        recurring    = true;   // true = EVERY, false = AT (once)
    long long   intervalSecs = 0;      // seconds between runs (EVERY)
    bool        hasAt        = false;  // EVERY n DAY AT 'HH:MM:SS'
    std::string atTime;                // "HH:MM:SS" (hasAt) or "YYYY-MM-DD HH:MM:SS" (once)
    std::time_t nextRun      = 0;
    std::string sql;
    bool        enabled      = true;
};

// ── EventScheduler ───────────────────────────────────────────
class EventScheduler {
public:
    using ExecFn = std::function<void(const std::string&)>;

    explicit EventScheduler(ExecFn fn, const std::string& eventsFile = "database.events")
        : execFn_(std::move(fn))
        , eventsFile_(eventsFile)
        , running_(false)
        , schedulerOn_(true)
    {}

    ~EventScheduler() { stop(); }

    // ── Lifecycle ────────────────────────────────────────────
    void start() {
        if (running_.exchange(true)) return;
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        running_.store(false);
        if (thread_.joinable()) thread_.join();
    }

    bool isOn()   const { return schedulerOn_.load(); }
    void setOn(bool v)  { schedulerOn_.store(v); }

    // ── CRUD ─────────────────────────────────────────────────
    void createEvent(const EventDef& def) {
        std::lock_guard<std::mutex> lk(mu_);
        events_.erase(std::remove_if(events_.begin(), events_.end(),
            [&](const EventDef& e) { return e.name == def.name; }),
            events_.end());
        events_.push_back(def);
        saveEvents_();
    }

    bool dropEvent(const std::string& name) {
        std::lock_guard<std::mutex> lk(mu_);
        auto before = events_.size();
        events_.erase(std::remove_if(events_.begin(), events_.end(),
            [&](const EventDef& e) { return e.name == name; }),
            events_.end());
        bool dropped = events_.size() < before;
        if (dropped) saveEvents_();
        return dropped;
    }

    bool setEventEnabled(const std::string& name, bool enabled) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& e : events_) {
            if (e.name == name) {
                e.enabled = enabled;
                saveEvents_();
                return true;
            }
        }
        return false;
    }

    std::vector<EventDef> getEvents() const {
        std::lock_guard<std::mutex> lk(mu_);
        return events_;
    }

    // ── Persistence ──────────────────────────────────────────
    // Format per line: name\trecurring\tintervalSecs\thasAt\tatTime\tnextRun\tenabled\tsql
    void loadEvents() {
        std::lock_guard<std::mutex> lk(mu_);
        std::ifstream f(eventsFile_);
        if (!f) return;
        events_.clear();
        std::string line;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            // Split on \t, sql is last field (may contain any non-\t chars)
            std::vector<std::string> parts;
            std::string cur;
            int field = 0;
            for (char c : line) {
                if (c == '\t' && field < 7) {
                    parts.push_back(cur); cur.clear(); ++field;
                } else { cur += c; }
            }
            parts.push_back(cur);
            if (parts.size() < 8) continue;
            EventDef ev;
            ev.name         = parts[0];
            ev.recurring    = parts[1] == "1";
            try { ev.intervalSecs = std::stoll(parts[2]); } catch (...) {}
            ev.hasAt        = parts[3] == "1";
            ev.atTime       = parts[4];
            try { ev.nextRun = static_cast<std::time_t>(std::stoll(parts[5])); } catch (...) {}
            ev.enabled      = parts[6] == "1";
            ev.sql          = parts[7];
            events_.push_back(ev);
        }
    }

    // ── Schedule string for display ──────────────────────────
    static std::string scheduleStr(const EventDef& ev) {
        if (!ev.recurring) {
            return "AT '" + ev.atTime + "'";
        }
        std::string unitStr;
        long long n = ev.intervalSecs;
        if (n % 2592000 == 0 && n >= 2592000) {
            unitStr = std::to_string(n / 2592000) + " MONTH";
        } else if (n % 604800 == 0 && n >= 604800) {
            unitStr = std::to_string(n / 604800) + " WEEK";
        } else if (n % 86400 == 0 && n >= 86400) {
            unitStr = std::to_string(n / 86400) + " DAY";
        } else if (n % 3600 == 0 && n >= 3600) {
            unitStr = std::to_string(n / 3600) + " HOUR";
        } else if (n % 60 == 0 && n >= 60) {
            unitStr = std::to_string(n / 60) + " MINUTE";
        } else {
            unitStr = std::to_string(n) + " SECOND";
        }
        std::string s = "EVERY " + unitStr;
        if (ev.hasAt) s += " AT '" + ev.atTime + "'";
        return s;
    }

    // ── nextRun helpers (public static for dispatch) ─────────
    static std::time_t computeNextRunRecurring(long long intervalSecs,
                                               bool hasAt, const std::string& atTime) {
        auto now = std::time(nullptr);
        if (!hasAt) {
            return now + intervalSecs;
        }
        // EVERY n DAY AT 'HH:MM:SS' — find next occurrence of that time
        int hh = 0, mm = 0, ss = 0;
        parseTimeHMS(atTime, hh, mm, ss);
        // Try today first
        std::tm ltm = milansql::safe_localtime(&now);
        ltm.tm_hour = hh; ltm.tm_min = mm; ltm.tm_sec = ss;
        ltm.tm_isdst = -1;
        std::time_t candidate = std::mktime(&ltm);
        if (candidate <= now) {
            // Past today — add one interval
            candidate += intervalSecs;
        }
        return candidate;
    }

    static std::time_t computeNextRunOnce(const std::string& atDateTime) {
        // Parse "YYYY-MM-DD HH:MM:SS"
        struct tm tm_ = {};
        if (atDateTime.size() >= 19) {
            try {
                tm_.tm_year = std::stoi(atDateTime.substr(0,  4)) - 1900;
                tm_.tm_mon  = std::stoi(atDateTime.substr(5,  2)) - 1;
                tm_.tm_mday = std::stoi(atDateTime.substr(8,  2));
                tm_.tm_hour = std::stoi(atDateTime.substr(11, 2));
                tm_.tm_min  = std::stoi(atDateTime.substr(14, 2));
                tm_.tm_sec  = std::stoi(atDateTime.substr(17, 2));
            } catch (...) {}
        }
        tm_.tm_isdst = -1;
        return std::mktime(&tm_);
    }

private:
    ExecFn               execFn_;
    std::string          eventsFile_;
    std::atomic<bool>    running_;
    std::atomic<bool>    schedulerOn_;
    std::thread          thread_;
    mutable std::mutex   mu_;
    std::vector<EventDef> events_;

    void saveEvents_() {
        // Caller holds mu_
        std::ofstream f(eventsFile_);
        if (!f) return;
        for (const auto& ev : events_) {
            f << ev.name         << "\t"
              << (ev.recurring ? "1" : "0") << "\t"
              << ev.intervalSecs << "\t"
              << (ev.hasAt ? "1" : "0") << "\t"
              << ev.atTime       << "\t"
              << static_cast<long long>(ev.nextRun) << "\t"
              << (ev.enabled ? "1" : "0") << "\t"
              << ev.sql          << "\n";
        }
    }

    void run() {
        while (running_.load()) {
            if (schedulerOn_.load()) {
                auto now = std::time(nullptr);
                std::vector<std::string> toExec;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    bool changed = false;
                    for (auto& ev : events_) {
                        if (!ev.enabled) continue;
                        if (ev.nextRun <= now) {
                            toExec.push_back(ev.sql);
                            if (ev.recurring) {
                                calcNextRun_(ev);
                            } else {
                                ev.enabled = false;  // one-shot: disable after execution
                            }
                            changed = true;
                        }
                    }
                    if (changed) saveEvents_();
                }
                for (const auto& sql : toExec) {
                    try { execFn_(sql); } catch (...) {}
                }
            }
            // Sleep 1 second in 100ms chunks for responsive stop
            for (int i = 0; i < 10 && running_.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void calcNextRun_(EventDef& ev) {
        // Caller holds mu_
        if (!ev.recurring) return;
        auto now = std::time(nullptr);
        if (!ev.hasAt) {
            ev.nextRun = now + ev.intervalSecs;
        } else {
            int hh = 0, mm = 0, ss = 0;
            parseTimeHMS(ev.atTime, hh, mm, ss);
            std::tm ltm = milansql::safe_localtime(&now);
            ltm.tm_hour = hh; ltm.tm_min = mm; ltm.tm_sec = ss;
            ltm.tm_isdst = -1;
            std::time_t candidate = std::mktime(&ltm);
            if (candidate <= now) candidate += ev.intervalSecs;
            ev.nextRun = candidate;
        }
    }

    static void parseTimeHMS(const std::string& s, int& hh, int& mm, int& ss) {
        if (s.size() >= 8) {
            try { hh = std::stoi(s.substr(0, 2)); } catch (...) { hh = 0; }
            try { mm = std::stoi(s.substr(3, 2)); } catch (...) { mm = 0; }
            try { ss = std::stoi(s.substr(6, 2)); } catch (...) { ss = 0; }
        }
    }
};

// ── Global pointer (set by main.cpp) ─────────────────────────
inline EventScheduler* g_eventScheduler = nullptr;

} // namespace milansql
