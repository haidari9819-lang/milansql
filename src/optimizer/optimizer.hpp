#pragma once
// ============================================================
// optimizer.hpp — Cost-Based Query Optimizer for MilanSQL
// Phase 48: Join-Reihenfolge, Index-Auswahl
// Optimizer Phase 3 (Konsolidierung): alle Kosten kommen aus
// cost_model.hpp; Zeilen-/Selektivitaetsschaetzungen aus
// g_tableStats (Phase 1/2). Die alte sqrt(rowCount)-Heuristik
// und die Kartesisch-Produkt-Kosten sind entfernt.
// ============================================================

#include <string>
#include <vector>
#include <algorithm>

#include "../engine/engine.hpp"
#include "../parser/parser.hpp"
#include "plan_selector.hpp"   // bringt cost_model.hpp + table_stats.hpp mit

namespace milansql {

struct OptimizationNote {
    std::string step;        // e.g. "Join-Reihenfolge", "Index-Auswahl"
    std::string original;    // original plan description
    std::string optimized;   // optimized plan description
    double costBefore = 0.0;
    double costAfter  = 0.0;
};

class QueryOptimizer {
public:
    // Perform all optimizations on the command, return optimization notes
    std::vector<OptimizationNote> optimize(ParsedCommand& cmd, Engine& engine) {
        std::vector<OptimizationNote> notes;

        // Only optimize SELECT commands
        if (cmd.type != CommandType::SELECT) return notes;

        // 1. Join order optimization (only if there are joins)
        if (!cmd.joinClauses.empty()) {
            auto note = optimizeJoinOrder(cmd, engine);
            if (!note.step.empty()) notes.push_back(note);
        }

        // 2. Index selection (best index for WHERE conditions)
        {
            auto note = selectBestIndex(cmd, engine);
            if (!note.step.empty()) notes.push_back(note);
        }

        return notes;
    }

    // Estimate row count for a table: echte Stats (ANALYZE) zuerst,
    // sonst tatsaechlicher rowCount, sonst 1000.
    static size_t estimateRowCount(const std::string& tableName, Engine& engine) {
        try {
            std::string key = engine.resolveTableName(tableName);
            const TableStats* ts = g_tableStats.getStats(key);
            if (ts && ts->rowCount > 0) return ts->rowCount;
            auto it = engine.tables_.find(key);
            if (it == engine.tables_.end()) return 1000;
            return it->second.rowCount();
        } catch (...) {
            return 1000;
        }
    }

    // Optimize join order: swap main table and first INNER JOIN table
    // if the joined table is smaller (fewer rows = smaller outer loop).
    // Kosten in der Note kommen aus dem CostModel (SeqScan beider
    // Seiten + HashJoin), nicht mehr aus |R|·|S|.
    static OptimizationNote optimizeJoinOrder(ParsedCommand& cmd, Engine& engine) {
        OptimizationNote note;
        if (cmd.joinClauses.empty()) return note;

        // Only optimize INNER JOINs — LEFT/RIGHT/FULL have semantic order constraints
        if (cmd.joinClauses[0].joinType != "INNER") return note;

        size_t mainCount = estimateRowCount(cmd.tableName, engine);
        size_t joinCount = estimateRowCount(cmd.joinClauses[0].table, engine);

        // Only swap if the join table is strictly smaller
        if (joinCount >= mainCount) return note;

        // CostModel-Kosten beider Reihenfolgen (HashJoin, Build auf inner)
        std::string keyMain = engine.resolveTableName(cmd.tableName);
        std::string keyJoin = engine.resolveTableName(cmd.joinClauses[0].table);
        auto inM = CostModel::tableInput(keyMain,
            static_cast<double>(mainCount), 64.0);
        auto inJ = CostModel::tableInput(keyJoin,
            static_cast<double>(joinCount), 64.0);
        CostEstimate sM = CostModel::seqScan(inM, 0, inM.rows);
        CostEstimate sJ = CostModel::seqScan(inJ, 0, inJ.rows);

        // Join-Selektivitaet aus ndv der ON-Spalten (1/max(ndv)),
        // ohne Stats Fallback 0.1 (CostModel::joinRows).
        auto colOf = [](const std::string& s) -> std::string {
            auto p = s.rfind('.');
            return p != std::string::npos ? s.substr(p + 1) : s;
        };
        auto ndvOf = [](const std::string& tbl, const std::string& col) -> double {
            const TableStats* ts = g_tableStats.getStats(tbl);
            if (!ts) return 0.0;
            auto it = ts->cols.find(col);
            return it != ts->cols.end()
                ? static_cast<double>(it->second.distinctCount) : 0.0;
        };
        double ndvL = ndvOf(keyMain, colOf(cmd.joinClauses[0].onLeft));
        double ndvR = ndvOf(keyJoin, colOf(cmd.joinClauses[0].onRight));
        double outRows = CostModel::joinRows(sM.rows, sJ.rows, ndvL, ndvR);

        // Record original state
        note.step       = "Join-Reihenfolge";
        note.original   = cmd.tableName + " -> " + cmd.joinClauses[0].table;
        note.costBefore = CostModel::hashJoin(sM, sJ, outRows).total;

        std::string oldMain = cmd.tableName;
        std::string oldJoin = cmd.joinClauses[0].table;

        // Swap table names
        cmd.tableName              = oldJoin;
        cmd.joinClauses[0].table   = oldMain;

        // Swap ON condition sides so semantics are preserved
        std::swap(cmd.joinClauses[0].onLeft, cmd.joinClauses[0].onRight);

        note.optimized  = cmd.tableName + " -> " + cmd.joinClauses[0].table;
        note.costAfter  = CostModel::hashJoin(sJ, sM, outRows).total;
        note.optimized += " (" + cmd.tableName + ": " + std::to_string(joinCount) +
                          " Zeilen, " + cmd.joinClauses[0].table + ": " +
                          std::to_string(mainCount) + " Zeilen)";

        return note;
    }

    // Select best index for WHERE conditions on the main table.
    // Phase 3: kostenbasiert via PlanSelector/CostModel — nur wenn
    // echte Stats (ANALYZE) vorliegen; die sqrt-Heuristik ist weg.
    static OptimizationNote selectBestIndex(ParsedCommand& cmd, Engine& engine) {
        OptimizationNote note;
        if (cmd.whereConds.empty()) return note;
        if (cmd.tableName.empty())  return note;

        std::string key = engine.resolveTableName(cmd.tableName);
        const TableStats* ts = g_tableStats.getStats(key);
        if (!ts || ts->rowCount == 0) return note;  // ohne Stats keine Kostenbasis

        // Tabellen-Prefix von Spalten strippen ("gross.klein_id" -> "klein_id")
        std::vector<WhereCondition> conds = cmd.whereConds;
        for (auto& wc : conds) {
            auto dotPos = wc.col.find('.');
            if (dotPos != std::string::npos)
                wc.col = wc.col.substr(dotPos + 1);
        }

        auto candidates = PlanSelector::indexesFromStats(key);
        if (candidates.empty()) return note;

        PlanChoice pc = PlanSelector::choose(key, conds, candidates);
        if (pc.planType == "SeqScan" || pc.indexName.empty()) return note;

        auto in = CostModel::tableInput(key,
            static_cast<double>(ts->rowCount), 64.0);
        CostEstimate seq = CostModel::seqScan(in, conds.size(), pc.estimatedRows);

        note.step       = "Index-Auswahl";
        note.original   = "FULL SCAN auf " + cmd.tableName;
        note.costBefore = seq.total;
        note.optimized  = pc.indexName + " auf " + cmd.tableName +
                          " (Spalte: " + pc.indexCol +
                          ", Selektivitaet: " +
                          std::to_string(static_cast<int>(pc.selectivity * 100)) + "%)";
        note.costAfter  = pc.estimatedCost;

        return note;
    }
};

} // namespace milansql
