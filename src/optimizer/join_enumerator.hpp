#pragma once
// ============================================================
// join_enumerator.hpp — Optimizer Phase 3: Join Enumeration
// (Selinger-DP, left-deep). Ersetzt dp_planner.hpp (Phase 113).
//
// EINZIGE Kostenquelle: cost_model.hpp (CostModel/g_costConsts).
// Join-Kardinalitaet aus echten Stats via CostModel::joinRows:
//   |R|·|S| / max(ndv(a.x), ndv(b.y)); ohne Stats Fallback 0.1.
//
//   - <= MAX_DP_TABLES Tabellen: Bitmask-DP ueber alle Subsets,
//     die die Basistabelle enthalten; pro Subset wird nur der
//     billigste LEFT-DEEP-Plan behalten (keine Bushy Trees).
//   - >  MAX_DP_TABLES: Greedy — jeweils die verbundene Tabelle
//     mit den geringsten Schrittkosten (CostModel) anfuegen.
//   - Cross-Products werden vermieden, solange Join-Praedikate
//     existieren (requiredTables-Konnektivitaet); erst wenn
//     keine verbundene Tabelle mehr uebrig ist, darf Greedy
//     eine unverbundene nehmen.
//   - Kosten pro Join-Schritt: cost(outer) + cost(inner) +
//     join_cost(Methode, rows) — Methode = min(Hash, NestLoop).
//   - Plan-Cache (Key: sortierte Tabellennamen) + invalidate().
//
// Referenz: Selinger et al. (1979), "Access Path Selection".
// ============================================================

#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <numeric>
#include <algorithm>
#include <sstream>
#include <iomanip>

#include "join_plan_types.hpp"
#include "cost_model.hpp"

namespace milansql {

class JoinEnumerator {
public:
    // Ab dieser Tabellenzahl kippt DP (2^n Subsets) in Greedy.
    static constexpr int MAX_DP_TABLES = 8;

    // tables[0] = Basistabelle, tables[1..n-1] ↔ JoinClauses[0..n-2]
    JoinPlan plan(const std::vector<JoinTableInfo>& tables,
                  const std::string& cacheKey = "") {
        const int n = static_cast<int>(tables.size());

        // ── Trivialfaelle ─────────────────────────────────────
        if (n <= 1) {
            JoinPlan p; p.dpUsed = false;
            p.description = "(single table)";
            return p;
        }
        if (n == 2) {
            JoinPlan p; p.joinOrder = {0}; p.dpUsed = false;
            CostEstimate l = scanOf(tables[0]);
            CostEstimate r = scanOf(tables[1]);
            double out = joinOutRows(l.rows, tables, 1, r.rows);
            p.estimatedCost = joinStep(l, r, out).total;
            p.description = "(2 tables, single join)";
            return p;
        }

        // ── Plan-Cache ────────────────────────────────────────
        if (!cacheKey.empty()) {
            std::lock_guard<std::mutex> g(cacheMu_);
            auto it = planCache_.find(cacheKey);
            if (it != planCache_.end()) {
                g_dpStats().planCacheHits.fetch_add(1, std::memory_order_relaxed);
                JoinPlan cached = it->second;
                cached.description = "[cached] " + cached.description;
                return cached;
            }
        }
        g_dpStats().planCacheMisses.fetch_add(1, std::memory_order_relaxed);

        JoinPlan result = (n <= MAX_DP_TABLES) ? planDp(tables)
                                               : planGreedy(tables);

        g_dpStats().queriesPlanned.fetch_add(1, std::memory_order_relaxed);
        g_dpStats().totalSubsetsEval.fetch_add(
            static_cast<uint64_t>(result.subsetsEvaluated),
            std::memory_order_relaxed);

        if (!cacheKey.empty() && result.dpUsed) {
            std::lock_guard<std::mutex> g(cacheMu_);
            planCache_[cacheKey] = result;
        }
        return result;
    }

    // ── Cache-Invalidierung ─────────────────────────────────────
    void invalidate() {
        std::lock_guard<std::mutex> g(cacheMu_);
        planCache_.clear();
    }
    void invalidate(const std::string& tableName) {
        std::lock_guard<std::mutex> g(cacheMu_);
        for (auto it = planCache_.begin(); it != planCache_.end(); ) {
            if (it->first.find(tableName) != std::string::npos)
                it = planCache_.erase(it);
            else
                ++it;
        }
    }
    size_t cacheSize() const {
        std::lock_guard<std::mutex> g(cacheMu_);
        return planCache_.size();
    }

    // ── Kosten-Bausteine (oeffentlich → unit-testbar) ───────────

    // ndv einer Spalte aus g_tableStats (0 = unbekannt)
    static double ndvOf(const std::string& tbl, const std::string& col) {
        const TableStats* ts = g_tableStats.getStats(tbl);
        if (!ts) return 0.0;
        auto it = ts->cols.find(col);
        return (it != ts->cols.end())
            ? static_cast<double>(it->second.distinctCount) : 0.0;
    }

    // SeqScan-Kosten einer Eingangstabelle (Stats, sonst rowCount)
    static CostEstimate scanOf(const JoinTableInfo& t) {
        auto in = CostModel::tableInput(
            t.name, static_cast<double>(t.rowCount), 64.0);
        return CostModel::seqScan(in, 0, in.rows);
    }

    // Ausgabe-Rows beim Anfuegen von tables[j] an ein Zwischen-
    // ergebnis mit leftRows Zeilen (ndv beider ON-Seiten).
    static double joinOutRows(double leftRows,
                              const std::vector<JoinTableInfo>& tables,
                              int j, double jRows) {
        const auto& tj = tables[static_cast<size_t>(j)];
        double rNdv = tj.joinColSelf.empty()
            ? 0.0 : ndvOf(tj.name, tj.joinColSelf);
        double lNdv = 0.0;
        if (tj.otherIdx >= 0 &&
            tj.otherIdx < static_cast<int>(tables.size()) &&
            !tj.joinColOther.empty())
            lNdv = ndvOf(tables[static_cast<size_t>(tj.otherIdx)].name,
                         tj.joinColOther);
        return CostModel::joinRows(leftRows, jRows, lNdv, rNdv);
    }

    // Kosten eines Join-Schritts: billigere Methode Hash vs. NestLoop.
    // outer.total enthaelt bereits alle Kosten des Zwischenergebnisses.
    static CostEstimate joinStep(const CostEstimate& outer,
                                 const CostEstimate& inner,
                                 double outRows) {
        CostEstimate h  = CostModel::hashJoin(outer, inner, outRows);
        CostEstimate nl = CostModel::nestedLoop(outer, inner, outRows);
        return (nl.total < h.total) ? nl : h;
    }

    // ── Block 3: Join-Methoden-Wahl (kostenbasiert) ─────────────
    // Entscheidet pro Join-Schritt zwischen HashJoin und Indexed-
    // Nested-Loop. Ergebnis:
    //   "hash"                → HashJoin (Default fuer Equi-Joins)
    //   "nested_loop_indexed" → NL ueber Index der inneren Seite
    //   ""                    → keine Stats: alte Heuristik behalten
    // Indexed-NL kommt nur infrage, wenn die innere Seite einen
    // Index auf der Join-Spalte hat UND die outer-Seite unter
    // g_nlThreshold (SET NL_THRESHOLD = n) geschaetzten Rows liegt.
    static std::string chooseJoinMethod(const std::string& rightTable,
                                        const std::string& rightCol,
                                        double outerRows,
                                        double rightRows,
                                        bool   rightHasIndex) {
        const TableStats* ts = g_tableStats.getStats(rightTable);
        if (!ts || ts->rowCount == 0)
            return "";  // ohne Stats keine Kostenbasis → alte Heuristik
        if (!rightHasIndex || outerRows >= g_nlThreshold)
            return "hash";

        auto in = CostModel::tableInput(rightTable, rightRows, 64.0);

        double ndv = ndvOf(rightTable, rightCol);
        double sel = (ndv >= 1.0) ? 1.0 / ndv : 0.1;
        double outRows = CostModel::joinRows(outerRows, in.rows, ndv, ndv);

        // Hash: SeqScan der inneren Seite + Build + Probe
        CostEstimate outer{0.0, 0.0, outerRows, 64.0};
        CostEstimate innerScan = CostModel::seqScan(in, 0, in.rows);
        double hashCost = CostModel::hashJoin(outer, innerScan, outRows).total;

        // Indexed-NL: pro outer-Row ein Index-Lookup
        double lookupCost = CostModel::indexScan(in, sel).total;
        double nlCost = outerRows * lookupCost +
                        outRows * g_costConsts.cpu_tuple_cost;

        return (nlCost < hashCost) ? "nested_loop_indexed" : "hash";
    }

private:
    mutable std::mutex              cacheMu_;
    std::map<std::string, JoinPlan> planCache_;

    // Konnektivitaet: alle requiredTables von j liegen im Subset?
    static bool connected(const JoinTableInfo& tj, int subsetMask) {
        for (int req : tj.requiredTables)
            if (!(subsetMask & (1 << req))) return false;
        return true;
    }

    // ── Selinger-DP (bitmask, left-deep) ────────────────────────
    JoinPlan planDp(const std::vector<JoinTableInfo>& tables) {
        const int n = static_cast<int>(tables.size());
        struct DpState {
            double           cost    = 1e18;
            double           estRows = 0.0;
            double           width   = 0.0;
            std::vector<int> order;   // JC-Indizes (tables-Index - 1)
            bool             valid   = false;
        };

        const int fullMask = (1 << n) - 1;
        std::vector<DpState> memo(static_cast<size_t>(fullMask + 1));
        size_t subsetsEval = 0;

        // Basis: Single-Table-Scans
        std::vector<CostEstimate> scans(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            scans[static_cast<size_t>(i)] = scanOf(tables[static_cast<size_t>(i)]);
            DpState& s = memo[static_cast<size_t>(1 << i)];
            s.cost    = scans[static_cast<size_t>(i)].total;
            s.estRows = scans[static_cast<size_t>(i)].rows;
            s.width   = scans[static_cast<size_t>(i)].width;
            s.valid   = true;
            ++subsetsEval;
        }

        // Bottom-up: groessere Subsets, Basistabelle (Bit 0) immer dabei
        for (int sz = 2; sz <= n; ++sz) {
            for (int mask = 1; mask <= fullMask; ++mask) {
                if (__builtin_popcount(static_cast<unsigned>(mask)) != sz) continue;
                if (!(mask & 1)) continue;
                ++subsetsEval;

                DpState& cur = memo[static_cast<size_t>(mask)];

                for (int j = 1; j < n; ++j) {
                    if (!(mask & (1 << j))) continue;
                    int submask = mask ^ (1 << j);
                    const DpState& sub = memo[static_cast<size_t>(submask)];
                    if (!sub.valid) continue;
                    if (!connected(tables[static_cast<size_t>(j)], submask))
                        continue;  // Cross-Product vermeiden

                    CostEstimate outer{0.0, sub.cost, sub.estRows, sub.width};
                    const CostEstimate& inner = scans[static_cast<size_t>(j)];
                    double outRows = joinOutRows(sub.estRows, tables, j, inner.rows);
                    CostEstimate step = joinStep(outer, inner, outRows);

                    if (!cur.valid || step.total < cur.cost) {
                        cur.cost    = step.total;
                        cur.estRows = step.rows;
                        cur.width   = step.width;
                        cur.valid   = true;
                        cur.order   = sub.order;
                        cur.order.push_back(j - 1);
                    }
                }
            }
        }

        JoinPlan result;
        result.subsetsEvaluated = subsetsEval;
        result.dpUsed           = true;

        const DpState& best = memo[static_cast<size_t>(fullMask)];
        if (best.valid && static_cast<int>(best.order.size()) == n - 1) {
            result.joinOrder     = best.order;
            result.estimatedCost = best.cost;
            std::ostringstream oss;
            oss << "Selinger DP: " << n << " tables, " << subsetsEval
                << " subsets, est.cost="
                << std::fixed << std::setprecision(1) << best.cost
                << "  order: " << tables[0].name;
            for (int idx : result.joinOrder)
                oss << " \u2192 " << tables[static_cast<size_t>(idx + 1)].name;
            result.description = oss.str();
        } else {
            // Fallback: Original-Reihenfolge (Join-Graph nicht verbunden)
            result.joinOrder.resize(static_cast<size_t>(n - 1));
            std::iota(result.joinOrder.begin(), result.joinOrder.end(), 0);
            result.dpUsed        = false;
            result.estimatedCost = 0.0;
            result.description   =
                "(fallback: original order — connectivity constraint)";
        }
        return result;
    }

    // ── Greedy-Fallback fuer > MAX_DP_TABLES ────────────────────
    JoinPlan planGreedy(const std::vector<JoinTableInfo>& tables) {
        const int n = static_cast<int>(tables.size());
        std::vector<CostEstimate> scans(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            scans[static_cast<size_t>(i)] = scanOf(tables[static_cast<size_t>(i)]);

        int joinedMask = 1;  // Basistabelle
        CostEstimate cur = scans[0];
        double totalCost = cur.total;
        std::vector<int> order;
        size_t evaluated = 1;

        while (static_cast<int>(order.size()) < n - 1) {
            int    bestJ    = -1;
            double bestCost = 1e18;
            CostEstimate bestStep;
            bool anyConnected = false;

            // Zwei Durchgaenge: erst nur verbundene Kandidaten,
            // Cross-Products nur wenn nichts verbunden ist.
            for (int pass = 0; pass < 2 && bestJ < 0; ++pass) {
                for (int j = 1; j < n; ++j) {
                    if (joinedMask & (1 << j)) continue;
                    bool conn = connected(tables[static_cast<size_t>(j)], joinedMask);
                    if (pass == 0 && !conn) continue;
                    if (pass == 0) anyConnected = true;
                    ++evaluated;

                    CostEstimate outer{0.0, totalCost, cur.rows, cur.width};
                    const CostEstimate& inner = scans[static_cast<size_t>(j)];
                    double outRows =
                        joinOutRows(cur.rows, tables, j, inner.rows);
                    CostEstimate step = joinStep(outer, inner, outRows);
                    if (step.total < bestCost) {
                        bestCost = step.total;
                        bestJ    = j;
                        bestStep = step;
                    }
                }
                if (pass == 0 && anyConnected && bestJ < 0) break;
            }
            if (bestJ < 0) break;  // sollte nicht passieren

            joinedMask |= (1 << bestJ);
            totalCost   = bestStep.total;
            cur.rows    = bestStep.rows;
            cur.width   = bestStep.width;
            order.push_back(bestJ - 1);
        }

        JoinPlan result;
        result.subsetsEvaluated = evaluated;
        if (static_cast<int>(order.size()) == n - 1) {
            result.joinOrder     = order;
            result.estimatedCost = totalCost;
            result.dpUsed        = true;
            std::ostringstream oss;
            oss << "Greedy (" << n << " tables > DP limit "
                << MAX_DP_TABLES << "), est.cost="
                << std::fixed << std::setprecision(1) << totalCost;
            result.description = oss.str();
        } else {
            result.joinOrder.resize(static_cast<size_t>(n - 1));
            std::iota(result.joinOrder.begin(), result.joinOrder.end(), 0);
            result.dpUsed      = false;
            result.description = "(fallback: original order)";
        }
        return result;
    }
};

// ── Globaler Singleton ─────────────────────────────────────────
inline JoinEnumerator& g_joinEnumerator() {
    static JoinEnumerator e;
    return e;
}

// ── Hook-Installation (Muster wie g_indexPathAdvisor) ──────────
inline const bool g_joinPlanHookInstalled = []() {
    g_joinPlanHook = [](const std::vector<JoinTableInfo>& tables,
                        const std::string& cacheKey) -> JoinPlan {
        return g_joinEnumerator().plan(tables, cacheKey);
    };
    g_joinMethodAdvisor = [](const std::string& rightTable,
                             const std::string& rightCol,
                             double outerRows, double rightRows,
                             bool rightHasIndex) -> std::string {
        return JoinEnumerator::chooseJoinMethod(
            rightTable, rightCol, outerRows, rightRows, rightHasIndex);
    };
    return true;
}();

} // namespace milansql
