#pragma once
// ============================================================
// auto_analyze.hpp — Optimizer Phase 3 (Block 4): Auto-ANALYZE
//
// Postgres-Logik: pro Tabelle ein Aenderungszaehler
// (INSERT/UPDATE/DELETE); ueberschreiten die Aenderungen seit
// dem letzten ANALYZE auto_analyze_threshold (Default 0.2 =
// 20 %) des damaligen rowCounts, wird die Tabelle im
// Hintergrund neu analysiert (Thread-Muster wie VacuumManager,
// nicht blockierend).
//
// Bewusst OHNE Abhaengigkeit auf table_stats/engine, damit
// engine.hpp diesen Header frueh einbinden kann (gleiches
// Muster wie join_plan_types.hpp). Der eigentliche Sweep
// (g_tableStats.analyzeTable) wird als Callback von aussen
// (main.cpp / http_server.hpp) injiziert.
// ============================================================

#include <string>
#include <map>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <functional>
#include <cstddef>
#include <ctime>

namespace milansql {

class AutoAnalyzeTracker {
public:
    static constexpr int DEFAULT_INTERVAL_SEC = 60;

    ~AutoAnalyzeTracker() { stop(); }

    // ── Konfiguration (SET AUTO_ANALYZE_ENABLED/THRESHOLD) ─────
    std::atomic<bool>   enabled{true};
    std::atomic<double> threshold{0.2};   // Anteil geaenderter Rows

    // Schluessel-Normalisierung: Engine-intern heissen Tabellen
    // "public.x", ANALYZE/g_tableStats fuehren sie aber als "x".
    // Damit Zaehler und Stats denselben Schluessel nutzen, wird
    // das Default-Schema-Praefix gestrippt.
    static std::string normKey(const std::string& tbl) {
        return tbl.rfind("public.", 0) == 0 ? tbl.substr(7) : tbl;
    }

    // ── Aenderungszaehler ───────────────────────────────────────
    void recordChange(const std::string& tbl, size_t n = 1) {
        if (!enabled.load(std::memory_order_relaxed)) return;
        std::lock_guard<std::mutex> g(mu_);
        changes_[normKey(tbl)] += n;
    }

    // Nach (Auto-)ANALYZE: Zaehler nullen, Referenz-Rows merken
    void resetAfterAnalyze(const std::string& tbl, size_t rows) {
        std::lock_guard<std::mutex> g(mu_);
        changes_[normKey(tbl)]       = 0;
        rowsAtAnalyze_[normKey(tbl)] = rows;
    }

    void forget(const std::string& tbl) {   // DROP TABLE
        std::lock_guard<std::mutex> g(mu_);
        changes_.erase(normKey(tbl));
        rowsAtAnalyze_.erase(normKey(tbl));
    }

    size_t changesFor(const std::string& tbl) const {
        std::lock_guard<std::mutex> g(mu_);
        auto it = changes_.find(normKey(tbl));
        return it == changes_.end() ? 0 : it->second;
    }

    // Trigger: Aenderungen > threshold * rowCount beim letzten
    // ANALYZE. Nie analysierte Tabellen: jede Aenderung triggert
    // (Referenz 0 → beim ersten Sweep werden Stats aufgebaut).
    bool needsAnalyze(const std::string& tbl) const {
        if (!enabled.load(std::memory_order_relaxed)) return false;
        std::lock_guard<std::mutex> g(mu_);
        auto it = changes_.find(normKey(tbl));
        if (it == changes_.end() || it->second == 0) return false;
        auto rit = rowsAtAnalyze_.find(normKey(tbl));
        double refRows = (rit != rowsAtAnalyze_.end())
            ? static_cast<double>(rit->second) : 0.0;
        return static_cast<double>(it->second) >
               threshold.load(std::memory_order_relaxed) * refRows;
    }

    // ── Status (SHOW AUTO ANALYZE STATUS) ───────────────────────
    void recordRun(size_t tablesAnalyzed) {
        std::lock_guard<std::mutex> g(mu_);
        ++runs_;
        tablesAnalyzed_ += tablesAnalyzed;
        lastRunUnix_ = std::time(nullptr);
    }
    size_t runs() const           { std::lock_guard<std::mutex> g(mu_); return runs_; }
    size_t tablesAnalyzed() const { std::lock_guard<std::mutex> g(mu_); return tablesAnalyzed_; }
    std::time_t lastRunUnix() const { std::lock_guard<std::mutex> g(mu_); return lastRunUnix_; }
    int intervalSeconds() const   { return intervalSec_; }

    // ── Hintergrund-Thread (Muster: VacuumManager) ──────────────
    // sweepFn: analysiert alle Tabellen mit needsAnalyze() und
    // liefert die Anzahl analysierter Tabellen zurueck.
    void start(std::function<size_t()> sweepFn,
               int intervalSec = DEFAULT_INTERVAL_SEC) {
        if (running_) return;
        running_     = true;
        intervalSec_ = intervalSec;
        thread_ = std::thread([this, fn = std::move(sweepFn), intervalSec]() {
            while (running_) {
                // Kurze Schlaf-Schritte → schnelles Stoppen moeglich
                for (int i = 0; i < intervalSec * 10 && running_; ++i)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (!running_) break;
                if (!enabled.load(std::memory_order_relaxed)) continue;
                size_t analyzed = fn();
                if (analyzed > 0) recordRun(analyzed);
            }
        });
    }

    void stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

    bool isRunning() const { return running_; }

private:
    mutable std::mutex            mu_;
    std::map<std::string, size_t> changes_;        // seit letztem ANALYZE
    std::map<std::string, size_t> rowsAtAnalyze_;  // rowCount beim ANALYZE

    size_t      runs_           = 0;
    size_t      tablesAnalyzed_ = 0;
    std::time_t lastRunUnix_    = 0;
    int         intervalSec_    = DEFAULT_INTERVAL_SEC;

    std::atomic<bool> running_{false};
    std::thread       thread_;
};

inline AutoAnalyzeTracker& g_autoAnalyze() {
    static AutoAnalyzeTracker t;
    return t;
}

} // namespace milansql
