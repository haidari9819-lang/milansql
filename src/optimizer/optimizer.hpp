#pragma once
// ============================================================
// optimizer.hpp — Cost-Based Query Optimizer for MilanSQL
// Phase 48: Join-Reihenfolge, Index-Auswahl, Predicate Pushdown
// ============================================================

#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

#include "../engine/engine.hpp"
#include "../parser/parser.hpp"

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

    // Estimate row count for a table (returns 1000 as default if unknown)
    static size_t estimateRowCount(const std::string& tableName, Engine& engine) {
        try {
            auto it = engine.tables_.find(tableName);
            if (it == engine.tables_.end()) return 1000;
            return it->second.rowCount();
        } catch (...) {
            return 1000;
        }
    }

    // Optimize join order: swap main table and first INNER JOIN table
    // if the joined table is smaller (fewer rows = smaller outer loop).
    static OptimizationNote optimizeJoinOrder(ParsedCommand& cmd, Engine& engine) {
        OptimizationNote note;
        if (cmd.joinClauses.empty()) return note;

        // Only optimize INNER JOINs — LEFT/RIGHT/FULL have semantic order constraints
        if (cmd.joinClauses[0].joinType != "INNER") return note;

        size_t mainCount = estimateRowCount(cmd.tableName, engine);
        size_t joinCount = estimateRowCount(cmd.joinClauses[0].table, engine);

        // Only swap if the join table is strictly smaller
        if (joinCount >= mainCount) return note;

        // Record original state
        note.step       = "Join-Reihenfolge";
        note.original   = cmd.tableName + " -> " + cmd.joinClauses[0].table;
        note.costBefore = static_cast<double>(mainCount) * static_cast<double>(joinCount);

        std::string oldMain = cmd.tableName;
        std::string oldJoin = cmd.joinClauses[0].table;

        // Swap table names
        cmd.tableName              = oldJoin;
        cmd.joinClauses[0].table   = oldMain;

        // Swap ON condition sides so semantics are preserved
        std::swap(cmd.joinClauses[0].onLeft, cmd.joinClauses[0].onRight);

        note.optimized  = cmd.tableName + " -> " + cmd.joinClauses[0].table;
        note.costAfter  = static_cast<double>(joinCount) * static_cast<double>(mainCount);
        note.optimized += " (" + cmd.tableName + ": " + std::to_string(joinCount) +
                          " Zeilen, " + cmd.joinClauses[0].table + ": " +
                          std::to_string(mainCount) + " Zeilen)";

        return note;
    }

    // Select best index for WHERE conditions on the main table.
    // Chooses the index with the highest estimated selectivity.
    static OptimizationNote selectBestIndex(ParsedCommand& cmd, Engine& engine) {
        OptimizationNote note;
        if (cmd.whereConds.empty()) return note;
        if (cmd.tableName.empty())  return note;

        auto it = engine.tables_.find(cmd.tableName);
        if (it == engine.tables_.end()) return note;
        const Table& tbl = it->second;

        size_t rowCount = tbl.rowCount();
        if (rowCount == 0) return note;

        // Find WHERE conditions that have an index on the main table
        std::string bestCol;
        std::string bestIdxName;
        double      bestSelectivity = 2.0; // lower is better (1.0 = full scan, <1.0 = selective)

        for (const auto& wc : cmd.whereConds) {
            if (wc.op != "=") continue;  // only equality for index lookup

            // Strip table prefix if present (e.g. "gross.klein_id" -> "klein_id")
            std::string colName = wc.col;
            auto dotPos = colName.find('.');
            if (dotPos != std::string::npos)
                colName = colName.substr(dotPos + 1);

            if (!tbl.hasIndex(colName)) continue;

            // Estimate selectivity: distinct values / row count
            // Use sqrt(rowCount) as a heuristic for distinct values
            size_t distinctEst = std::max(static_cast<size_t>(1),
                                          static_cast<size_t>(std::sqrt(static_cast<double>(rowCount))));
            double selectivity = static_cast<double>(distinctEst) /
                                 static_cast<double>(rowCount);

            if (selectivity < bestSelectivity) {
                bestSelectivity = selectivity;
                bestCol         = colName;
                // Find the index name for this column
                for (const auto& info : tbl.getIndexes()) {
                    std::string leading = info.colName;
                    auto comma = leading.find(',');
                    if (comma != std::string::npos)
                        leading = leading.substr(0, comma);
                    // trim spaces
                    while (!leading.empty() && leading.front() == ' ') leading.erase(leading.begin());
                    while (!leading.empty() && leading.back()  == ' ') leading.pop_back();
                    if (leading == colName) {
                        bestIdxName = info.indexName;
                        break;
                    }
                }
            }
        }

        if (bestCol.empty() || bestIdxName.empty()) return note;

        // Only report when we actually have an index to use
        note.step       = "Index-Auswahl";
        note.original   = "FULL SCAN auf " + cmd.tableName;
        note.costBefore = static_cast<double>(rowCount);
        note.optimized  = bestIdxName + " auf " + cmd.tableName +
                          " (Spalte: " + bestCol +
                          ", Selektivitaet: " +
                          std::to_string(static_cast<int>(bestSelectivity * 100)) + "%)";
        note.costAfter  = bestSelectivity * static_cast<double>(rowCount);

        return note;
    }
};

} // namespace milansql
