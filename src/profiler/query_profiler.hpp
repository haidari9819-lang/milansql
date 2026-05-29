#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <iostream>
#include <algorithm>

// ============================================================
// query_profiler.hpp — Phase 69: Query Profiler
// ============================================================

namespace milansql {

struct ProfileStep {
    std::string status;
    double      durationMs = 0.0;
};

struct ProfileEntry {
    int                      id;
    std::string              sql;
    double                   totalMs = 0.0;
    std::vector<ProfileStep> steps;
};

class QueryProfiler {
public:
    void enable()  { isEnabled_ = true; }
    void disable() { isEnabled_ = false; }
    bool isEnabled() const { return isEnabled_; }

    void startQuery(const std::string& sql) {
        if (!isEnabled_) return;
        inQuery_       = true;
        currentSql_    = sql;
        queryStart_    = now();
        stepStart_     = queryStart_;
        currentSteps_.clear();
    }

    void addStep(const std::string& status) {
        if (!isEnabled_ || !inQuery_) return;
        auto t = now();
        double ms = toMs(t - stepStart_);
        currentSteps_.push_back({status, ms});
        stepStart_ = t;
    }

    void endQuery() {
        if (!isEnabled_ || !inQuery_) return;
        inQuery_ = false;
        auto t = now();
        double totalMs = toMs(t - queryStart_);

        ProfileEntry entry;
        entry.id      = nextId_++;
        entry.sql     = currentSql_;
        entry.totalMs = totalMs;
        entry.steps   = currentSteps_;

        // ensure max 100 profiles (drop oldest)
        if (profiles_.size() >= 100)
            profiles_.erase(profiles_.begin());
        profiles_.push_back(std::move(entry));
    }

    void showProfiles() const {
        if (profiles_.empty()) {
            std::cout << "  (Keine Profile vorhanden — PROFILE ON aktivieren)\n\n";
            return;
        }
        // column widths
        size_t wSql = 5; // "Query"
        for (const auto& p : profiles_)
            wSql = std::max(wSql, p.sql.size() > 60 ? size_t(60) : p.sql.size());

        auto hline = [&](const char* l, const char* m, const char* r, const char* f) {
            std::cout << l;
            for (size_t j = 0; j < 6;     ++j) std::cout << f;  // ID
            std::cout << m;
            for (size_t j = 0; j < wSql + 2; ++j) std::cout << f;  // Query
            std::cout << m;
            for (size_t j = 0; j < 12;    ++j) std::cout << f;  // Duration
            std::cout << r << "\n";
        };
        auto cell = [](const std::string& s, size_t w) {
            std::cout << " " << s;
            for (size_t j = s.size(); j < w; ++j) std::cout << " ";
            std::cout << " \u2502";
        };
        auto fmtMs = [](double ms) -> std::string {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.3fms", ms);
            return buf;
        };

        std::cout << "\n";
        hline("  \u250c", "\u252c", "\u2510", "\u2500");
        std::cout << "  \u2502";
        cell("ID",        4);
        cell("Query",     wSql);
        cell("Duration",  10);
        std::cout << "\n";
        hline("  \u251c", "\u253c", "\u2524", "\u2500");

        for (const auto& p : profiles_) {
            std::string sqlDisp = p.sql.size() > 60 ? p.sql.substr(0, 57) + "..." : p.sql;
            std::cout << "  \u2502";
            cell(std::to_string(p.id), 4);
            cell(sqlDisp,              wSql);
            cell(fmtMs(p.totalMs),     10);
            std::cout << "\n";
        }
        hline("  \u2514", "\u2534", "\u2518", "\u2500");
        std::cout << "  " << profiles_.size() << " Profile\n\n";
    }

    void showProfile(int id) const {
        const ProfileEntry* entry = nullptr;
        for (const auto& p : profiles_)
            if (p.id == id) { entry = &p; break; }
        if (!entry) {
            std::cout << "  FEHLER: Kein Profil mit ID " << id << "\n\n";
            return;
        }

        size_t wStatus = 8;
        for (const auto& s : entry->steps)
            wStatus = std::max(wStatus, s.status.size());
        wStatus = std::max(wStatus, size_t(7)); // "Total  "

        auto hline = [&](const char* l, const char* m, const char* r, const char* f) {
            std::cout << l;
            for (size_t j = 0; j < wStatus + 2; ++j) std::cout << f;
            std::cout << m;
            for (size_t j = 0; j < 12; ++j) std::cout << f;
            std::cout << r << "\n";
        };
        auto cell = [](const std::string& s, size_t w) {
            std::cout << " " << s;
            for (size_t j = s.size(); j < w; ++j) std::cout << " ";
            std::cout << " \u2502";
        };
        auto fmtMs = [](double ms) -> std::string {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.3fms", ms);
            return buf;
        };

        std::cout << "\n  Query " << entry->id << ": " << entry->sql << "\n";
        hline("  \u250c", "\u252c", "\u2510", "\u2500");
        std::cout << "  \u2502";
        cell("Status",   wStatus);
        cell("Duration", 10);
        std::cout << "\n";
        hline("  \u251c", "\u253c", "\u2524", "\u2500");

        for (const auto& s : entry->steps) {
            std::cout << "  \u2502";
            cell(s.status,          wStatus);
            cell(fmtMs(s.durationMs), 10);
            std::cout << "\n";
        }
        // separator before Total
        hline("  \u251c", "\u253c", "\u2524", "\u2500");
        std::cout << "  \u2502";
        cell("Total",             wStatus);
        cell(fmtMs(entry->totalMs), 10);
        std::cout << "\n";
        hline("  \u2514", "\u2534", "\u2518", "\u2500");
        std::cout << "\n";
    }

    void clear() { profiles_.clear(); nextId_ = 1; }

private:
    using Clock = std::chrono::high_resolution_clock;
    using TP    = Clock::time_point;

    bool        isEnabled_ = false;
    bool        inQuery_   = false;
    int         nextId_    = 1;
    std::string currentSql_;
    std::vector<ProfileStep> currentSteps_;
    TP          queryStart_;
    TP          stepStart_;
    std::vector<ProfileEntry> profiles_;

    static TP   now()              { return Clock::now(); }
    static double toMs(Clock::duration d) {
        return std::chrono::duration<double, std::milli>(d).count();
    }
};

} // namespace milansql
