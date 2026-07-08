#pragma once
// ============================================================
// join_plan_types.hpp — Optimizer Phase 3: gemeinsame Typen
// fuer die Join-Enumeration (Selinger-DP in join_enumerator.hpp).
//
// Bewusst OHNE Abhaengigkeit auf table_stats/cost_model, damit
// engine.hpp diese Typen frueh einbinden kann (table_stats.hpp
// braucht WhereCondition aus engine.hpp — waere zirkulaer).
// Die eigentliche Enumeration wird ueber g_joinPlanHook
// injiziert; installiert am Ende von join_enumerator.hpp
// (gleiches Muster wie g_indexPathAdvisor / plan_selector.hpp).
// ============================================================

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <cstdint>

namespace milansql {

// ── JoinPlan: Ergebnis der Join-Enumeration ────────────────────
struct JoinPlan {
    std::vector<int> joinOrder;        // 0-basierte Indizes in JoinClauses[]
    double           estimatedCost   = 0.0;
    size_t           subsetsEvaluated = 0;
    bool             dpUsed          = false;  // true = Planner hat gueltige Order geliefert
    std::string      description;              // menschenlesbare Zusammenfassung
};

// ── Per-Tabellen-Info fuer den Planner ─────────────────────────
// tables[0] = Basistabelle, tables[1..n-1] = JoinClauses[0..n-2].
struct JoinTableInfo {
    std::string      name;             // aufgeloester (physischer) Name == Stats-Key
    size_t           rowCount = 0;
    std::vector<int> requiredTables;   // Indizes, die bereits im Ergebnis sein muessen

    // Phase 3: Join-Praedikat-Info fuer ndv-basierte Selektivitaet
    // (a.x = b.y → sel = 1/max(ndv(a.x), ndv(b.y)))
    std::string joinColSelf;           // Spalte dieser Tabelle im ON (ohne Prefix)
    int         otherIdx = -1;         // tables[]-Index der Gegenseite
    std::string joinColOther;          // Spalte der Gegenseite (ohne Prefix)
};

// ── Globale lock-freie Planner-Statistiken ─────────────────────
struct DpStats {
    std::atomic<uint64_t> queriesPlanned{0};   // Planner-Laeufe (>= 3 Tabellen)
    std::atomic<uint64_t> planCacheHits{0};
    std::atomic<uint64_t> planCacheMisses{0};
    std::atomic<uint64_t> totalSubsetsEval{0};
};
inline DpStats& g_dpStats() {
    static DpStats s;
    return s;
}

// ── Hook: von join_enumerator.hpp installiert ──────────────────
// Nicht gesetzt (z. B. Bench/Embedded ohne Dispatch-Header):
// executeJoins behaelt die Original-Reihenfolge bei.
inline std::function<JoinPlan(const std::vector<JoinTableInfo>&,
                              const std::string&)> g_joinPlanHook;

// ── Block 3: Join-Methoden-Wahl ────────────────────────────────
// NL_THRESHOLD: Indexed-Nested-Loop kommt nur infrage, wenn die
// outer-Seite weniger geschaetzte Rows hat (SET NL_THRESHOLD = n).
inline double g_nlThreshold = 1000.0;

// Advisor (installiert in join_enumerator.hpp):
// (rightTable, rightJoinCol, outerRows, rightRows, rightHasIndex)
//   → "hash" | "nested_loop_indexed" | "" (keine Stats → alte Heuristik)
inline std::function<std::string(const std::string&, const std::string&,
                                 double, double, bool)> g_joinMethodAdvisor;

} // namespace milansql
