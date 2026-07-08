#pragma once
#include <cmath>
#include <string>
#include <vector>
#include "table_stats.hpp"
// ============================================================
// cost_model.hpp — Optimizer-Roadmap Phase 2: Cost Model
//
// Einheitliches Kostenmodell auf Basis der echten Statistiken
// aus Phase 1 (g_tableStats). Konstanten analog zu Postgres:
//   seq_page_cost        = 1.0    (sequenzielle Page-Lesung)
//   random_page_cost     = 4.0    (Random-Access-Page, Index-Heap-Fetch)
//   cpu_tuple_cost       = 0.01   (Verarbeitung einer Row)
//   cpu_index_tuple_cost = 0.005  (Verarbeitung eines Index-Eintrags)
//   cpu_operator_cost    = 0.0025 (Auswertung eines Operators/Filters)
//
// Kostenformeln (vereinfacht, Postgres-analog):
//   SeqScan:   pages·seq_page + rows·cpu_tuple + rows·nFilter·cpu_op
//   IndexScan: log2(N)·cpu_op (B-Tree-Abstieg)
//              + matchedPages·random_page (Heap-Fetches)
//              + matched·(cpu_index_tuple + cpu_tuple)
//   HashJoin:  Build(inner) + Probe(outer) + outRows·cpu_tuple
//              startup = Kosten bis zur ersten Ausgabe-Row (= Build)
//   NestLoop:  outer + outerRows·inner + outRows·cpu_tuple
//
// Row-Width/Pages kommen aus ColumnStats.avgWidth (Phase 1);
// Fallback ohne Stats: Breite = nCols·8, Pages = rows·width/4096.
// Join-Kardinalität: |R|·|S| / max(ndv(R.k), ndv(S.k)) — Standard-
// Schätzer; ohne Spaltenstats Fallback-Selektivität 0.1 (wie
// dp_planner.hpp Phase 113).
// ============================================================

namespace milansql {

// ── Kostenkonstanten (Postgres-analog, via SHOW COST MODEL sichtbar) ──
struct CostConstants {
    double seq_page_cost        = 1.0;
    double random_page_cost     = 4.0;
    double cpu_tuple_cost       = 0.01;
    double cpu_index_tuple_cost = 0.005;
    double cpu_operator_cost    = 0.0025;
    double page_size_bytes      = 4096.0;
};
inline CostConstants g_costConsts;

// ── Kostenschätzung eines Plan-Knotens ─────────────────────────
struct CostEstimate {
    double startup = 0.0;  // Kosten bis zur ersten Row
    double total   = 0.0;  // Gesamtkosten
    double rows    = 0.0;  // geschätzte Ausgabe-Rows
    double width   = 0.0;  // geschätzte Row-Breite in Bytes
};

class CostModel {
public:
    // ── Basisdaten einer Tabelle (aus Stats, sonst Fallback) ──
    struct TableInput {
        double rows  = 0.0;
        double width = 0.0;
        double pages = 1.0;
        bool   fromStats = false;
    };

    static TableInput tableInput(const std::string& tbl,
                                 double fallbackRows,
                                 double fallbackWidth) {
        TableInput in;
        const TableStats* ts = g_tableStats.getStats(tbl);
        if (ts && ts->rowCount > 0 && !ts->cols.empty()) {
            in.rows = static_cast<double>(ts->rowCount);
            double w = 0.0;
            for (const auto& cv : ts->cols) w += cv.second.avgWidth;
            in.width = (w > 0.0) ? w : fallbackWidth;
            in.fromStats = true;
        } else {
            in.rows  = fallbackRows;
            in.width = fallbackWidth;
        }
        if (in.rows < 0.0)  in.rows = 0.0;
        if (in.width < 1.0) in.width = 1.0;
        in.pages = std::max(1.0,
            std::ceil(in.rows * in.width / g_costConsts.page_size_bytes));
        return in;
    }

    // ── Seq Scan ──────────────────────────────────────────────
    // outRows: erwartete Rows NACH Filter (Selektivität aus Phase 1);
    //          Filterkosten fallen trotzdem für ALLE Rows an.
    static CostEstimate seqScan(const TableInput& in,
                                size_t nFilterConds,
                                double outRows) {
        const auto& c = g_costConsts;
        CostEstimate e;
        e.startup = 0.0;
        e.total   = in.pages * c.seq_page_cost
                  + in.rows  * c.cpu_tuple_cost
                  + in.rows  * static_cast<double>(nFilterConds)
                             * c.cpu_operator_cost;
        e.rows  = (outRows >= 0.0) ? outRows : in.rows;
        e.width = in.width;
        return e;
    }

    // ── Index Scan (B-Tree, Equality/Range) ───────────────────
    // selectivity: Anteil der Rows, die der Index-Lookup trifft
    static CostEstimate indexScan(const TableInput& in,
                                  double selectivity) {
        const auto& c = g_costConsts;
        if (selectivity < 0.0) selectivity = 0.0;
        if (selectivity > 1.0) selectivity = 1.0;
        double matched      = in.rows * selectivity;
        double matchedPages = std::min(in.pages,
            std::max(1.0, std::ceil(matched * in.width / c.page_size_bytes)));
        double descend = std::log2(std::max(2.0, in.rows)) * c.cpu_operator_cost;
        CostEstimate e;
        e.startup = descend;
        e.total   = descend
                  + matchedPages * c.random_page_cost
                  + matched * (c.cpu_index_tuple_cost + c.cpu_tuple_cost);
        e.rows  = matched;
        e.width = in.width;
        return e;
    }

    // ── Join-Kardinalität: |R|·|S| / max(ndv_R, ndv_S) ────────
    // ndv = 0 → unbekannt → Fallback-Selektivität 0.1
    static double joinRows(double leftRows, double rightRows,
                           double leftNdv, double rightNdv) {
        double maxNdv = std::max(leftNdv, rightNdv);
        if (maxNdv >= 1.0)
            return (leftRows * rightRows) / maxNdv;
        return leftRows * rightRows * 0.1;
    }

    // ── Hash Join (Build auf inner, Probe auf outer) ──────────
    static CostEstimate hashJoin(const CostEstimate& outer,
                                 const CostEstimate& inner,
                                 double outRows) {
        const auto& c = g_costConsts;
        CostEstimate e;
        double build = inner.total + inner.rows * c.cpu_operator_cost;
        double probe = outer.total + outer.rows * c.cpu_operator_cost;
        e.startup = build;
        e.total   = build + probe + outRows * c.cpu_tuple_cost;
        e.rows    = outRows;
        e.width   = outer.width + inner.width;
        return e;
    }

    // ── Nested Loop Join ──────────────────────────────────────
    static CostEstimate nestedLoop(const CostEstimate& outer,
                                   const CostEstimate& inner,
                                   double outRows) {
        const auto& c = g_costConsts;
        CostEstimate e;
        e.startup = outer.startup + inner.startup;
        e.total   = outer.total
                  + outer.rows * (inner.total - inner.startup)
                  + outRows * c.cpu_tuple_cost;
        e.rows    = outRows;
        e.width   = outer.width + inner.width;
        return e;
    }
};

} // namespace milansql
