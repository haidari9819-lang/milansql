#pragma once
// ============================================================
// dispatch.hpp — Shared SQL command dispatch for MilanSQL
// Used by main.cpp (REPL) and server.hpp (TCP Server)
// Phase 47: extracted from main.cpp
// ============================================================

#include <iostream>
#include <sstream>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <limits>

#include "engine/engine.hpp"
#include "engine/btree.hpp"
#include "parser/parser.hpp"
#include "types/array_type.hpp"  // Phase 88: Array Data Type
#include "storage/storage.hpp"
#include "optimizer/optimizer.hpp"
#include "optimizer/query_rewriter.hpp"
#include "optimizer/adaptive_stats.hpp"
#include "optimizer/table_stats.hpp"   // Phase 86: Column Statistics
#include "backup/backup.hpp"
#include "server/pool_stats.hpp"
#include "replication/repl_state.hpp"
#include "utils/csv_utils.hpp"
#include "scheduler/event_scheduler.hpp"
#include "profiler/query_profiler.hpp"  // Phase 69: Query Profiler
#include "pubsub/pubsub.hpp"           // Phase 76: LISTEN/NOTIFY
#include "copy/copy_manager.hpp"       // Phase 92: COPY FROM/TO

namespace milansql {

// ── Phase 69: Global Query Profiler ──────────────────────────
static QueryProfiler g_profiler;

// ── Phase 82: Query Rewriter + Adaptive Stats ────────────────
static QueryRewriter g_queryRewriter;
static AdaptiveStats g_adaptiveStats;

// ── Phase 92: COPY FROM/TO — persistent stats ─────────────────
static CopyManager g_copyManager;

// ── Phase 86: Table Statistics Manager ───────────────────────
static TableStatsManager g_tableStats;

// ── Phase 59: Replication helpers ────────────────────────────
// Returns true if the current operation should be blocked (slave read-only)
static inline bool dispatch_slaveReadOnly() {
    return milansql::g_replState.isSlave.load()
        && !milansql::tl_binlogReplay;
}
// Appends sql to binlog (master mode only)
static inline void dispatch_binlogWrite(const std::string& sql) {
    if (milansql::g_replState.isMaster.load() && milansql::g_binlogHook)
        milansql::g_binlogHook(sql);
}

// ── Phase 67: Split multiple SQL statements ───────────────────
// Splits input on ';' respecting string literals, -- comments,
// /* */ block comments, and BEGIN...END depth (for procedures/triggers).
static inline std::vector<std::string> splitStatements(const std::string& input) {
    std::vector<std::string> stmts;
    std::string cur;
    int beginDepth = 0;
    bool inStr         = false;
    bool inLineCmt     = false;
    bool inBlockCmt    = false;

    auto isWordChar = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    };

    size_t i = 0;
    while (i < input.size()) {
        char c = input[i];

        // ── line comment --
        if (!inStr && !inBlockCmt && !inLineCmt &&
            c == '-' && i + 1 < input.size() && input[i+1] == '-') {
            inLineCmt = true; cur += c; i++; continue;
        }
        if (inLineCmt) {
            cur += c; if (c == '\n') inLineCmt = false; i++; continue;
        }

        // ── block comment /* */
        if (!inStr && !inBlockCmt &&
            c == '/' && i + 1 < input.size() && input[i+1] == '*') {
            inBlockCmt = true; cur += c; i++; continue;
        }
        if (inBlockCmt) {
            cur += c;
            if (c == '*' && i + 1 < input.size() && input[i+1] == '/') {
                cur += input[i+1]; i += 2; inBlockCmt = false;
            } else i++;
            continue;
        }

        // ── single-quoted string
        if (!inStr && c == '\'') { inStr = true; cur += c; i++; continue; }
        if (inStr) {
            cur += c;
            if (c == '\'' && i + 1 < input.size() && input[i+1] == '\'') {
                cur += input[i+1]; i += 2;   // '' escape
            } else if (c == '\'') {
                inStr = false; i++;
            } else { i++; }
            continue;
        }

        // ── BEGIN depth tracking
        if (i + 5 <= input.size()) {
            std::string w5;
            for (size_t j = i; j < i+5; ++j)
                w5 += static_cast<char>(std::toupper(static_cast<unsigned char>(input[j])));
            bool lOk = (i == 0 || !isWordChar(input[i-1]));
            bool rOk = (i+5 >= input.size() || !isWordChar(input[i+5]));
            if (w5 == "BEGIN" && lOk && rOk) {
                beginDepth++; cur += input.substr(i, 5); i += 5; continue;
            }
        }

        // ── END depth tracking (but not END IF / END LOOP / END WHILE / END CASE)
        if (beginDepth > 0 && i + 3 <= input.size()) {
            std::string w3;
            for (size_t j = i; j < i+3; ++j)
                w3 += static_cast<char>(std::toupper(static_cast<unsigned char>(input[j])));
            bool lOk = (i == 0 || !isWordChar(input[i-1]));
            bool rOk = (i+3 >= input.size() || !isWordChar(input[i+3]));
            if (w3 == "END" && lOk && rOk) {
                // peek at next word
                size_t nx = i + 3;
                while (nx < input.size() && input[nx] == ' ') ++nx;
                std::string nw;
                for (size_t j = nx; j < input.size() && std::isalpha(static_cast<unsigned char>(input[j])); ++j)
                    nw += static_cast<char>(std::toupper(static_cast<unsigned char>(input[j])));
                if (nw != "IF" && nw != "LOOP" && nw != "WHILE" && nw != "CASE") {
                    beginDepth--;
                }
                cur += input.substr(i, 3); i += 3; continue;
            }
        }

        // ── split on ';' at depth 0
        if (c == ';' && beginDepth == 0) {
            std::string s = cur;
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
            while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
            if (!s.empty()) stmts.push_back(s);
            cur.clear(); i++;
        } else {
            cur += c; i++;
        }
    }

    // trailing statement without semicolon
    while (!cur.empty() && std::isspace(static_cast<unsigned char>(cur.front()))) cur.erase(cur.begin());
    while (!cur.empty() && std::isspace(static_cast<unsigned char>(cur.back()))) cur.pop_back();
    if (!cur.empty()) stmts.push_back(cur);

    return stmts;
}

// ── Forward declarations of helper print functions ───────────
static inline void dispatch_printTable(const milansql::Table& tbl, int limit = -1, int offset = 0);
static inline void dispatch_printIndexes(const std::vector<milansql::IndexInfo>& indexes, const std::string& tableName);
static inline void dispatch_printExplain(const milansql::ExplainPlan& plan);
static inline std::string dispatch_whereDesc(const milansql::ParsedCommand& cmd);
static inline void dispatch_printDescribe(const milansql::Table& tbl);
static inline std::string dispatch_buildCreateTableSql(const milansql::Table& tbl);
static inline milansql::Table dispatch_materializeView(milansql::Engine& engine, milansql::Parser& parser,
    const std::string& viewName, const milansql::ParsedCommand& outerCmd);
static inline milansql::Table dispatch_executeSelectToTable(milansql::Engine& engine, milansql::Parser& parser,
    milansql::ParsedCommand cmd);

// ── Phase 88: UNNEST expansion ───────────────────────────────
// Expands any UNNEST(...) SelectItem: each row becomes N rows (one per element).
// Non-UNNEST columns keep their value in every expanded row.
static inline milansql::Table dispatch_expandUnnestResult(
        const milansql::Table& src,
        const std::vector<milansql::SelectItem>& items) {
    // Find the UNNEST item and its column index in src
    int unnestItemIdx = -1;
    for (int k = 0; k < static_cast<int>(items.size()); ++k) {
        if (items[static_cast<size_t>(k)].isUnnest) { unnestItemIdx = k; break; }
    }
    if (unnestItemIdx < 0) return src.clone();  // no UNNEST, nothing to do

    const milansql::SelectItem& unnestItem = items[static_cast<size_t>(unnestItemIdx)];
    // Determine column name in src (after projectWithItems the col is the alias or unnest col)
    std::string unnestColName = unnestItem.alias.empty() ? unnestItem.unnestCol : unnestItem.alias;

    // Find index in src columns
    int srcColIdx = -1;
    for (int k = 0; k < static_cast<int>(src.columns().size()); ++k) {
        if (src.columns()[static_cast<size_t>(k)].name == unnestColName) { srcColIdx = k; break; }
    }
    if (srcColIdx < 0) return src.clone();

    // Build result table with same columns
    milansql::Table result("", src.columns());
    for (const auto& row : src.rows()) {
        if (row.xmax != 0) continue;
        std::string arrVal = static_cast<size_t>(srcColIdx) < row.values.size()
                             ? row.values[static_cast<size_t>(srcColIdx)] : "";
        auto elems = milansql::ArrayUtils::parse(arrVal);
        if (elems.empty()) {
            // Keep the row with NULL for empty arrays
            milansql::Row newRow = row;
            if (static_cast<size_t>(srcColIdx) < newRow.values.size())
                newRow.values[static_cast<size_t>(srcColIdx)] = "NULL";
            result.insert(newRow);
        } else {
            for (const auto& elem : elems) {
                milansql::Row newRow = row;
                if (static_cast<size_t>(srcColIdx) < newRow.values.size())
                    newRow.values[static_cast<size_t>(srcColIdx)] = elem;
                result.insert(newRow);
            }
        }
    }
    return result;
}

// Entfernt äußere einfache Anführungszeichen für die Anzeige
static inline std::string dispatch_displayVal(const std::string& v) {
    if (v.size() >= 2 && v.front() == '\'' && v.back() == '\'')
        return v.substr(1, v.size() - 2);
    return v;
}

// ── printTable ────────────────────────────────────────────────
static inline void dispatch_printTable(const milansql::Table& tbl, int limit, int offset) {
    const auto& cols = tbl.columns();
    const auto& rows = tbl.rows();

    if (cols.empty()) { std::cout << "  (Tabelle hat keine Spalten)\n"; return; }

    size_t total      = rows.size();
    size_t startRow   = (offset > 0 && static_cast<size_t>(offset) < total)
                        ? static_cast<size_t>(offset) : 0;
    size_t remaining  = total - startRow;
    size_t printRows  = (limit >= 0 && static_cast<size_t>(limit) < remaining)
                        ? static_cast<size_t>(limit) : remaining;

    std::vector<size_t> widths;
    widths.reserve(cols.size());
    for (const auto& col : cols) widths.push_back(col.name.size());
    for (size_t r = startRow; r < startRow + printRows; ++r)
        for (size_t i = 0; i < rows[r].values.size() && i < widths.size(); ++i)
            widths[i] = std::max(widths[i], dispatch_displayVal(rows[r].values[i]).size());

    auto hline = [&](const std::string& l, const std::string& m,
                     const std::string& r, const std::string& f) {
        std::cout << l;
        for (size_t i = 0; i < widths.size(); ++i) {
            for (size_t j = 0; j < widths[i] + 2; ++j) std::cout << f;
            if (i + 1 < widths.size()) std::cout << m;
        }
        std::cout << r << "\n";
    };

    hline("\u250c", "\u252c", "\u2510", "\u2500");
    std::cout << "\u2502";
    for (size_t i = 0; i < cols.size(); ++i) {
        std::cout << " " << cols[i].name;
        for (size_t j = cols[i].name.size(); j < widths[i]; ++j) std::cout << " ";
        std::cout << " \u2502";
    }
    std::cout << "\n";
    hline("\u251c", "\u253c", "\u2524", "\u2500");

    if (printRows == 0) {
        std::cout << "\u2502";
        for (size_t i = 0; i < cols.size(); ++i)
            std::cout << "  " << std::string(widths[i], ' ') << "\u2502";
        std::cout << "\n";
    } else {
        for (size_t r = startRow; r < startRow + printRows; ++r) {
            std::cout << "\u2502";
            for (size_t i = 0; i < cols.size(); ++i) {
                const std::string val =
                    dispatch_displayVal((i < rows[r].values.size()) ? rows[r].values[i] : "");
                std::cout << " " << val;
                for (size_t j = val.size(); j < widths[i]; ++j) std::cout << " ";
                std::cout << " \u2502";
            }
            std::cout << "\n";
        }
    }
    hline("\u2514", "\u2534", "\u2518", "\u2500");

    if (limit >= 0 || offset > 0) {
        std::cout << "  " << printRows << " von " << total << " Zeile(n)";
        if (offset > 0) std::cout << " (OFFSET " << startRow << ")";
        if (limit >= 0) std::cout << " (LIMIT " << limit << ")";
        std::cout << "\n\n";
    } else {
        std::cout << "  " << printRows << " Zeile(n)\n\n";
    }
}

// ── printIndexes ──────────────────────────────────────────────
static inline void dispatch_printIndexes(const std::vector<milansql::IndexInfo>& indexes,
                     const std::string& tableName) {
    if (indexes.empty()) {
        std::cout << "  (Keine Indizes auf '" << tableName << "')\n\n"; return;
    }
    std::vector<std::string> hdr = {"Index-Name", "Spalte", "Typ"};
    std::vector<size_t> w = {hdr[0].size(), hdr[1].size(), hdr[2].size()};
    for (const auto& idx : indexes) {
        w[0] = std::max(w[0], idx.indexName.size());
        w[1] = std::max(w[1], idx.colName.size());
        w[2] = std::max(w[2], idx.type.size());
    }
    auto hline = [&](const std::string& l, const std::string& m,
                     const std::string& r, const std::string& f) {
        std::cout << l;
        for (size_t i = 0; i < 3; ++i) {
            for (size_t j = 0; j < w[i] + 2; ++j) std::cout << f;
            if (i + 1 < 3) std::cout << m;
        }
        std::cout << r << "\n";
    };
    hline("\u250c", "\u252c", "\u2510", "\u2500");
    std::cout << "\u2502";
    for (size_t i = 0; i < 3; ++i) {
        std::cout << " " << hdr[i];
        for (size_t j = hdr[i].size(); j < w[i]; ++j) std::cout << " ";
        std::cout << " \u2502";
    }
    std::cout << "\n";
    hline("\u251c", "\u253c", "\u2524", "\u2500");
    for (const auto& idx : indexes) {
        std::vector<std::string> row = {idx.indexName, idx.colName, idx.type};
        std::cout << "\u2502";
        for (size_t i = 0; i < 3; ++i) {
            std::cout << " " << row[i];
            for (size_t j = row[i].size(); j < w[i]; ++j) std::cout << " ";
            std::cout << " \u2502";
        }
        std::cout << "\n";
    }
    hline("\u2514", "\u2534", "\u2518", "\u2500");
    std::cout << "  " << indexes.size() << " Index(e) auf '" << tableName << "'\n\n";
}

// ── printExplain ──────────────────────────────────────────────
static inline void dispatch_printExplain(const milansql::ExplainPlan& plan) {
    if (plan.steps.empty()) {
        std::cout << "  (Kein Plan verfuegbar)\n\n"; return;
    }
    std::vector<std::string> hdr = {"Schritt", "Operation", "Tabelle", "Details", "Index"};
    std::vector<size_t> w(5);
    for (size_t i = 0; i < 5; ++i) w[i] = hdr[i].size();
    for (const auto& s : plan.steps) {
        w[0] = std::max(w[0], std::to_string(s.nr).size());
        w[1] = std::max(w[1], s.op.size());
        w[2] = std::max(w[2], s.table.size());
        w[3] = std::max(w[3], s.details.size());
        w[4] = std::max(w[4], s.index.size());
    }
    auto hline = [&](const std::string& l, const std::string& m,
                     const std::string& r, const std::string& f) {
        std::cout << l;
        for (size_t i = 0; i < 5; ++i) {
            for (size_t j = 0; j < w[i] + 2; ++j) std::cout << f;
            if (i + 1 < 5) std::cout << m;
        }
        std::cout << r << "\n";
    };
    auto printRow = [&](const std::vector<std::string>& cells) {
        std::cout << "\u2502";
        for (size_t i = 0; i < 5; ++i) {
            std::cout << " " << cells[i];
            for (size_t j = cells[i].size(); j < w[i]; ++j) std::cout << " ";
            std::cout << " \u2502";
        }
        std::cout << "\n";
    };
    hline("\u250c", "\u252c", "\u2510", "\u2500");
    printRow(hdr);
    hline("\u251c", "\u253c", "\u2524", "\u2500");
    for (const auto& s : plan.steps)
        printRow({std::to_string(s.nr), s.op, s.table, s.details, s.index});
    hline("\u2514", "\u2534", "\u2518", "\u2500");
    std::cout << "  EXPLAIN: " << plan.steps.size() << " Schritt(e)\n\n";
}

// ── whereDesc ─────────────────────────────────────────────────
static inline std::string dispatch_whereDesc(const milansql::ParsedCommand& cmd) {
    std::string s;
    for (size_t k = 0; k < cmd.whereConds.size(); ++k) {
        if (k > 0) s += " " + cmd.whereLogic + " ";
        s += cmd.whereConds[k].col + " " +
             cmd.whereConds[k].op  + " " +
             cmd.whereConds[k].val;
    }
    return s;
}

// ── printDescribe ─────────────────────────────────────────────
static inline void dispatch_printDescribe(const milansql::Table& tbl) {
    const auto& cols = tbl.columns();
    if (cols.empty()) { std::cout << "  (Keine Spalten)\n\n"; return; }

    std::vector<std::string> hdr = {"Name", "Typ", "NOT NULL", "UNIQUE", "DEFAULT", "PK", "AI", "CHECK", "GENERATED"};
    std::vector<size_t> w(9);
    for (size_t i = 0; i < 9; ++i) w[i] = hdr[i].size();
    for (const auto& col : cols) {
        w[0] = std::max(w[0], col.name.size());
        w[1] = std::max(w[1], col.type.size());
        w[4] = std::max(w[4], col.hasDefault ? col.defaultValue.size() : size_t(1));
        if (!col.checks.empty()) {
            std::string cs;
            for (const auto& cc : col.checks) {
                if (!cs.empty()) cs += " ";
                cs += cc.op + cc.val;
            }
            w[7] = std::max(w[7], cs.size());
        }
        if (col.isGenerated) {
            std::string gs = (col.isStored ? "STORED" : "VIRTUAL");
            gs += " AS (" + col.generatedExpr + ")";
            w[8] = std::max(w[8], gs.size());
        }
    }

    auto hline = [&](const std::string& l, const std::string& m,
                     const std::string& r, const std::string& f) {
        std::cout << l;
        for (size_t i = 0; i < 9; ++i) {
            for (size_t j = 0; j < w[i] + 2; ++j) std::cout << f;
            if (i + 1 < 9) std::cout << m;
        }
        std::cout << r << "\n";
    };
    auto cell = [](const std::string& s, size_t width) {
        std::cout << " " << s;
        for (size_t j = s.size(); j < width; ++j) std::cout << " ";
        std::cout << " \u2502";
    };

    std::cout << "\n  Tabelle: " << tbl.name() << "\n";
    hline("  \u250c", "\u252c", "\u2510", "\u2500");
    std::cout << "  \u2502";
    for (size_t i = 0; i < 9; ++i) cell(hdr[i], w[i]);
    std::cout << "\n";
    hline("  \u251c", "\u253c", "\u2524", "\u2500");

    for (const auto& col : cols) {
        std::string checkStr = "-";
        if (!col.checks.empty()) {
            checkStr = "";
            for (const auto& cc : col.checks) {
                if (!checkStr.empty()) checkStr += " ";
                checkStr += cc.op + cc.val;
            }
        }
        std::string genStr = "-";
        if (col.isGenerated)
            genStr = (col.isStored ? "STORED" : "VIRTUAL") +
                     std::string(" AS (") + col.generatedExpr + ")";
        std::cout << "  \u2502";
        cell(col.name,                               w[0]);
        cell(col.type,                               w[1]);
        cell(col.notNull      ? "YES" : "NO",        w[2]);
        cell(col.isUnique     ? "YES" : "NO",        w[3]);
        cell(col.hasDefault   ? col.defaultValue:"-",w[4]);
        cell(col.isPrimaryKey ? "YES" : "NO",        w[5]);
        cell(col.autoIncrement? "YES" : "NO",        w[6]);
        cell(checkStr,                               w[7]);
        cell(genStr,                                 w[8]);
        std::cout << "\n";
    }
    hline("  \u2514", "\u2534", "\u2518", "\u2500");
    std::cout << "  " << cols.size() << " Spalte(n)\n";

    const auto& fks = tbl.getForeignKeys();
    if (!fks.empty()) {
        std::vector<std::string> fhdr = {"Spalte", "Ref. Tabelle", "Ref. Spalte", "ON DELETE"};
        std::vector<size_t> fw = {fhdr[0].size(), fhdr[1].size(),
                                   fhdr[2].size(), fhdr[3].size()};
        for (const auto& fk : fks) {
            fw[0] = std::max(fw[0], fk.fromCol.size());
            fw[1] = std::max(fw[1], fk.refTable.size());
            fw[2] = std::max(fw[2], fk.refCol.size());
            fw[3] = std::max(fw[3], fk.onDelete.size());
        }
        auto fhline = [&](const std::string& l, const std::string& m,
                          const std::string& r, const std::string& f) {
            std::cout << l;
            for (size_t i = 0; i < 4; ++i) {
                for (size_t j = 0; j < fw[i] + 2; ++j) std::cout << f;
                if (i + 1 < 4) std::cout << m;
            }
            std::cout << r << "\n";
        };
        std::cout << "\n  Foreign Keys:\n";
        fhline("  \u250c", "\u252c", "\u2510", "\u2500");
        std::cout << "  \u2502";
        for (size_t i = 0; i < 4; ++i) cell(fhdr[i], fw[i]);
        std::cout << "\n";
        fhline("  \u251c", "\u253c", "\u2524", "\u2500");
        for (const auto& fk : fks) {
            std::cout << "  \u2502";
            cell(fk.fromCol,  fw[0]);
            cell(fk.refTable, fw[1]);
            cell(fk.refCol,   fw[2]);
            cell(fk.onDelete, fw[3]);
            std::cout << "\n";
        }
        fhline("  \u2514", "\u2534", "\u2518", "\u2500");
    }
    std::cout << "\n";
}

// ── buildCreateTableSql ───────────────────────────────────────
static inline std::string dispatch_buildCreateTableSql(const milansql::Table& tbl) {
    const auto& cols = tbl.columns();
    const auto& fks  = tbl.getForeignKeys();

    std::string sql = "CREATE TABLE " + tbl.name() + " (";
    for (size_t i = 0; i < cols.size(); ++i) {
        if (i > 0) sql += ", ";
        const auto& c = cols[i];
        sql += c.name + " " + c.type;
        if (c.isPrimaryKey)  sql += " PRIMARY KEY";
        if (c.autoIncrement) sql += " AUTO_INCREMENT";
        if (c.notNull && !c.isPrimaryKey) sql += " NOT NULL";
        if (c.isUnique && !c.isPrimaryKey) sql += " UNIQUE";
        if (c.hasDefault)    sql += " DEFAULT " + c.defaultValue;
        for (const auto& cc : c.checks)
            sql += " CHECK (" + c.name + " " + cc.op + " " + cc.val + ")";
        if (c.isGenerated)
            sql += " GENERATED ALWAYS AS (" + c.generatedExpr + ")" +
                   (c.isStored ? " STORED" : " VIRTUAL");
    }
    for (const auto& fk : fks) {
        sql += ", FOREIGN KEY (" + fk.fromCol + ") REFERENCES "
             + fk.refTable + "(" + fk.refCol + ")";
        if (fk.onDelete != "RESTRICT")
            sql += " ON DELETE " + fk.onDelete;
    }
    sql += ")";
    return sql;
}

// ── materializeView ───────────────────────────────────────────
static inline milansql::Table dispatch_materializeView(
        milansql::Engine& engine,
        milansql::Parser& parser,
        const std::string& viewName,
        const milansql::ParsedCommand& outerCmd)
{
    const std::string& vsql = engine.getViewSql(viewName);
    milansql::ParsedCommand vc = parser.parse(vsql);

    for (const auto& sq : vc.subqueries) {
        if (sq.condIdx < vc.whereConds.size()) {
            vc.whereConds[sq.condIdx].inList =
                engine.subqueryValues(
                    sq.subTable, sq.subCol,
                    sq.subWhere, sq.subWhereLogic);
        }
    }

    milansql::Table base;
    if (!vc.whereConds.empty()) {
        auto qr = engine.selectWhere(vc.tableName, vc.whereConds, vc.whereLogic);
        base = std::move(qr.table);
    } else {
        base = engine.selectAll(vc.tableName).clone();
    }

    if (!vc.selectColumns.empty())
        base = base.project(vc.selectColumns);

    if (!outerCmd.whereConds.empty())
        base = engine.filterTable(base, outerCmd.whereConds, outerCmd.whereLogic);

    return base;
}

// ── executeSelectToTable ──────────────────────────────────────
static inline milansql::Table dispatch_executeSelectToTable(
        milansql::Engine& engine,
        milansql::Parser& parser,
        milansql::ParsedCommand cmd)
{
    for (const auto& sq : cmd.subqueries) {
        if (sq.condIdx < cmd.whereConds.size()) {
            cmd.whereConds[sq.condIdx].inList =
                engine.subqueryValues(sq.subTable, sq.subCol,
                                      sq.subWhere, sq.subWhereLogic);
        }
    }

    if (cmd.isSetOp) {
        milansql::Table leftResult;
        if (!cmd.whereConds.empty()) {
            auto qr = engine.selectWhere(cmd.tableName, cmd.whereConds, cmd.whereLogic);
            leftResult = std::move(qr.table);
        } else {
            leftResult = engine.selectAll(cmd.tableName).clone();
        }
        if (!cmd.selectColumns.empty())
            leftResult = leftResult.project(cmd.selectColumns);

        milansql::ParsedCommand rc = parser.parse(cmd.rightSql);
        for (const auto& sq : rc.subqueries) {
            if (sq.condIdx < rc.whereConds.size())
                rc.whereConds[sq.condIdx].inList =
                    engine.subqueryValues(sq.subTable, sq.subCol,
                                          sq.subWhere, sq.subWhereLogic);
        }
        milansql::Table rightResult;
        if (!rc.whereConds.empty()) {
            auto qr = engine.selectWhere(rc.tableName, rc.whereConds, rc.whereLogic);
            rightResult = std::move(qr.table);
        } else {
            rightResult = engine.selectAll(rc.tableName).clone();
        }
        if (!rc.selectColumns.empty())
            rightResult = rightResult.project(rc.selectColumns);

        milansql::Table result = engine.executeSetOp(leftResult, cmd.setOp, rightResult);
        if (!cmd.orderByCols.empty()) result.sortByMulti(cmd.orderByCols);
        return result;
    }

    if (cmd.isJoin) {
        // Phase 89: Register any foreign tables as temp tables before join
        {
            std::vector<std::string> fdwToClean;
            auto registerFdw = [&](const std::string& tbl) {
                if (engine.isForeignTable(tbl)) {
                    milansql::Table ft = engine.executeForeignScan(tbl);
                    engine.registerTempTable(tbl, std::move(ft));
                    fdwToClean.push_back(tbl);
                }
            };
            registerFdw(cmd.tableName);
            for (const auto& jc : cmd.joinClauses) registerFdw(jc.table);

            // Phase 48: silently optimize join order before execution
            {
                milansql::QueryOptimizer qopt;
                qopt.optimize(cmd, engine);
            }
            auto result = engine.executeJoins(
                cmd.tableName, cmd.joinClauses,
                cmd.whereConds, cmd.whereLogic);

            // Clean up foreign table temp registrations
            for (const auto& tbl : fdwToClean)
                engine.dropTempTable(tbl);

            if (!cmd.orderByCols.empty()) result.sortByMulti(cmd.orderByCols);
            if (!cmd.selectColumns.empty())
                result = result.project(cmd.selectColumns);
            return result;
        }
    }

    if (cmd.isGroupBy) {
        auto result = engine.groupBy(
            cmd.tableName,
            cmd.whereConds, cmd.whereLogic,
            cmd.groupByCols,
            cmd.selectItems,
            cmd.havingConds, cmd.havingLogic);
        if (!cmd.orderByCols.empty()) result.sortByMulti(cmd.orderByCols);
        return result;
    }

    // Phase 89: Foreign Data Wrapper — intercept SELECT on foreign tables
    if (engine.isForeignTable(cmd.tableName)) {
        milansql::Table foreignResult = engine.executeForeignScan(cmd.tableName);
        // Apply WHERE filter on the in-memory result
        if (!cmd.whereConds.empty())
            foreignResult = engine.filterTable(foreignResult, cmd.whereConds, cmd.whereLogic);
        if (!cmd.orderByCols.empty()) foreignResult.sortByMulti(cmd.orderByCols);
        if (!cmd.selectColumns.empty())
            foreignResult = foreignResult.project(cmd.selectColumns);
        if (cmd.isDistinct) foreignResult.makeDistinct();
        if (cmd.limit >= 0) {
            milansql::Table lim(foreignResult.name(), foreignResult.columns());
            size_t off = (cmd.limitOffset > 0) ? static_cast<size_t>(cmd.limitOffset) : 0;
            const auto& allRows = foreignResult.rows();
            size_t count = 0;
            for (size_t ri = off; ri < allRows.size() && count < static_cast<size_t>(cmd.limit); ++ri, ++count)
                lim.insert(allRows[ri]);
            return lim;
        }
        return foreignResult;
    }

    milansql::Table result;
    if (!cmd.whereConds.empty()) {
        auto qr = engine.selectWhere(cmd.tableName, cmd.whereConds, cmd.whereLogic);
        result = std::move(qr.table);
    } else {
        result = engine.selectAllFiltered(cmd.tableName); // Phase 75: RLS
    }

    if (cmd.hasCaseItems && !cmd.selectItems.empty()) {
        bool hasWin = false;
        bool hasUnnest = false;
        for (const auto& si : cmd.selectItems) {
            if (si.isWindowFunc) { hasWin = true; break; }
            if (si.isUnnest) hasUnnest = true;
        }
        if (hasWin)
            result = engine.projectWithWindowItems(result, cmd.selectItems);
        else
            result = engine.projectWithItems(result, cmd.selectItems);
        // Phase 88: expand UNNEST after projection
        if (hasUnnest)
            result = dispatch_expandUnnestResult(result, cmd.selectItems);
        if (!cmd.orderByCols.empty()) result.sortByMulti(cmd.orderByCols);
    } else {
        if (!cmd.orderByCols.empty()) result.sortByMulti(cmd.orderByCols);
        if (!cmd.selectColumns.empty())
            result = result.project(cmd.selectColumns);
    }
    if (cmd.isDistinct) result.makeDistinct();
    return result;
}

// ── Phase 62: Partition persistence ──────────────────────────
// Format: tableName\ttype\tcolumn\thashCount\tRANGES:name=limitStr,...\tLISTS:name=v1|v2,...
inline void dispatch_savePartitions(const milansql::Engine& engine,
                                    const std::string& path = "database.partitions") {
    // Gather all tables with partitions by iterating SHOW command output
    // We save directly from engine via a known list; use a helper approach:
    // Write one line per table with partition info.
    // We access tables via engine's public showPartitions which also tells us they exist.
    // Since we don't have a "listPartitionedTables" method, we'll use a workaround:
    // iterate via engine's showTables and check each.
    std::ofstream out(path);
    if (!out.is_open()) return;
    // Get all tables
    for (const auto& tblName : engine.getAllTableNames()) {
        try {
            const milansql::PartitionInfo& pi = engine.getTablePartitionInfo(tblName);
            if (!pi.hasPartitions()) continue;
            std::string typeStr;
            if      (pi.type == milansql::PartitionType::RANGE) typeStr = "RANGE";
            else if (pi.type == milansql::PartitionType::LIST)  typeStr = "LIST";
            else if (pi.type == milansql::PartitionType::HASH)  typeStr = "HASH";
            else continue;
            out << tblName << "\t" << typeStr << "\t" << pi.column << "\t" << pi.hashCount;
            if (pi.type == milansql::PartitionType::RANGE) {
                out << "\tRANGES:";
                for (size_t i = 0; i < pi.ranges.size(); ++i) {
                    if (i) out << ",";
                    out << pi.ranges[i].name << "=" << pi.ranges[i].limitStr;
                }
            } else if (pi.type == milansql::PartitionType::LIST) {
                out << "\tLISTS:";
                for (size_t i = 0; i < pi.lists.size(); ++i) {
                    if (i) out << ",";
                    out << pi.lists[i].name << "=";
                    for (size_t j = 0; j < pi.lists[i].values.size(); ++j) {
                        if (j) out << "|";
                        out << pi.lists[i].values[j];
                    }
                }
            } else {
                out << "\t";
            }
            out << "\n";
        } catch (...) {}
    }
}

inline void dispatch_loadPartitions(milansql::Engine& engine,
                                    const std::string& path = "database.partitions") {
    std::ifstream in(path);
    if (!in.is_open()) return;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        // Split by tab
        std::vector<std::string> fields;
        std::string cur;
        for (char c : line) {
            if (c == '\t') { fields.push_back(cur); cur.clear(); }
            else cur += c;
        }
        fields.push_back(cur);
        if (fields.size() < 4) continue;
        std::string tbl      = fields[0];
        std::string typeStr  = fields[1];
        std::string col      = fields[2];
        int hashCount = 0;
        try { hashCount = std::stoi(fields[3]); } catch (...) {}
        milansql::PartitionInfo pi;
        pi.column = col;
        pi.hashCount = hashCount;
        if (typeStr == "RANGE") {
            pi.type = milansql::PartitionType::RANGE;
            if (fields.size() >= 5 && fields[4].substr(0, 7) == "RANGES:") {
                std::string rdata = fields[4].substr(7);
                // split by comma
                std::stringstream ss(rdata);
                std::string token;
                while (std::getline(ss, token, ',')) {
                    auto eq = token.find('=');
                    if (eq == std::string::npos) continue;
                    milansql::PartitionRangeDef r;
                    r.name = token.substr(0, eq);
                    r.limitStr = token.substr(eq + 1);
                    if (r.limitStr == "MAXVALUE")
                        r.limit = std::numeric_limits<long long>::max();
                    else { try { r.limit = std::stoll(r.limitStr); } catch (...) { r.limit = 0; } }
                    pi.ranges.push_back(r);
                }
            }
        } else if (typeStr == "LIST") {
            pi.type = milansql::PartitionType::LIST;
            if (fields.size() >= 5 && fields[4].substr(0, 6) == "LISTS:") {
                std::string ldata = fields[4].substr(6);
                std::stringstream ss(ldata);
                std::string token;
                while (std::getline(ss, token, ',')) {
                    auto eq = token.find('=');
                    if (eq == std::string::npos) continue;
                    milansql::PartitionListDef l;
                    l.name = token.substr(0, eq);
                    std::string vdata = token.substr(eq + 1);
                    std::stringstream sv(vdata);
                    std::string val;
                    while (std::getline(sv, val, '|'))
                        l.values.push_back(val);
                    pi.lists.push_back(l);
                }
            }
        } else if (typeStr == "HASH") {
            pi.type = milansql::PartitionType::HASH;
        }
        try { engine.setTablePartitionInfo(tbl, pi); } catch (...) {}
    }
}

// ── Phase 66: Procedure Interpreter (DECLARE/CURSOR/LOOP/IF) ──────────────────
namespace {

struct ProcState {
    std::map<std::string, std::string> vars;
    struct CursorSt {
        std::string sql;
        std::vector<std::vector<std::string>> rows;
        std::vector<std::string> colNames;
        size_t pos = 0;
        bool isOpen = false;
    };
    std::map<std::string, CursorSt> cursors;
    std::string notFoundVar;
    std::string notFoundVal = "1";
    bool hasNotFoundHandler = false;
};

struct ProcExec {
    ProcState& state;
    milansql::Engine& engine;
    std::function<void()>& persistFn;

    static std::string pu(std::string s) {
        for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }
    static std::string ptrim(std::string s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        return s;
    }
    static std::string stripQ(const std::string& s) {
        if (s.size() >= 2 &&
            ((s.front()=='\'' && s.back()=='\'') ||
             (s.front()=='"'  && s.back()=='"')))
            return s.substr(1, s.size()-2);
        return s;
    }
    static bool isAlnumU(char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
    }
    static bool wbLeft(const std::string& s, size_t pos) {
        return pos == 0 || !isAlnumU(s[pos-1]);
    }
    static bool wbRight(const std::string& s, size_t pos, size_t len) {
        return (pos + len >= s.size()) || !isAlnumU(s[pos + len]);
    }

    // Whole-word variable substitution
    std::string subst(const std::string& sql) const {
        std::string result = sql;
        for (const auto& [varName, varVal] : state.vars) {
            std::string out;
            size_t pos = 0;
            while (pos < result.size()) {
                size_t found = result.find(varName, pos);
                if (found == std::string::npos) { out += result.substr(pos); break; }
                if (wbLeft(result, found) && wbRight(result, found, varName.size())) {
                    out += result.substr(pos, found - pos) + varVal;
                    pos = found + varName.size();
                } else {
                    out += result.substr(pos, found - pos + 1);
                    pos = found + 1;
                }
            }
            result = out;
        }
        return result;
    }

    // Resolve a token: local var lookup, else literal
    std::string resolve(const std::string& tok) const {
        auto it = state.vars.find(tok);
        if (it != state.vars.end()) return it->second;
        return tok;
    }

    // Split procedure body into logical statements.
    // depth++ on LOOP or THEN, depth-- on END LOOP or END IF.
    // Split on ';' only at depth==0.
    static std::vector<std::string> splitStmts(const std::string& body) {
        std::vector<std::string> stmts;
        std::string cur;
        int depth = 0;
        size_t i = 0;
        while (i < body.size()) {
            if (wbLeft(body, i)) {
                if (i + 8 <= body.size() && pu(body.substr(i, 8)) == "END LOOP" && wbRight(body, i, 8)) {
                    cur += body.substr(i, 8); i += 8; depth--; continue;
                }
                if (i + 6 <= body.size() && pu(body.substr(i, 6)) == "END IF" && wbRight(body, i, 6)) {
                    cur += body.substr(i, 6); i += 6; depth--; continue;
                }
                if (i + 4 <= body.size() && pu(body.substr(i, 4)) == "LOOP" && wbRight(body, i, 4)) {
                    cur += body.substr(i, 4); i += 4; depth++; continue;
                }
                if (i + 4 <= body.size() && pu(body.substr(i, 4)) == "THEN" && wbRight(body, i, 4)) {
                    cur += body.substr(i, 4); i += 4; depth++; continue;
                }
            }
            if (body[i] == ';' && depth == 0) {
                auto s = ptrim(cur);
                if (!s.empty()) stmts.push_back(s);
                cur.clear(); i++;
            } else {
                cur += body[i++];
            }
        }
        auto s = ptrim(cur);
        if (!s.empty()) stmts.push_back(s);
        return stmts;
    }

    // Evaluate simple condition: lhs op rhs (with var resolution)
    bool evalCond(const std::string& cond) const {
        std::vector<std::string> toks;
        std::istringstream iss(cond);
        std::string t;
        while (iss >> t) toks.push_back(t);
        if (toks.size() == 1) {
            std::string v = stripQ(resolve(toks[0]));
            return !v.empty() && v != "0";
        }
        if (toks.size() < 3) return false;
        std::string lhs = stripQ(resolve(toks[0]));
        const std::string& op = toks[1];
        std::string rhs = stripQ(resolve(toks[2]));
        try {
            double lv = std::stod(lhs), rv = std::stod(rhs);
            if (op == "=")  return lv == rv;
            if (op == "!=" || op == "<>") return lv != rv;
            if (op == "<")  return lv <  rv;
            if (op == ">")  return lv >  rv;
            if (op == "<=") return lv <= rv;
            if (op == ">=") return lv >= rv;
        } catch (...) {}
        if (op == "=")  return lhs == rhs;
        if (op == "!=" || op == "<>") return lhs != rhs;
        if (op == "<")  return lhs <  rhs;
        if (op == ">")  return lhs >  rhs;
        if (op == "<=") return lhs <= rhs;
        if (op == ">=") return lhs >= rhs;
        return false;
    }

    // Execute a SQL statement with variable substitution
    void execSql(const std::string& rawSql) {
        std::string sql = subst(rawSql);
        milansql::Parser p2;
        milansql::ParsedCommand sc = p2.parse(sql);
        for (const auto& sq : sc.subqueries) {
            if (sq.condIdx < sc.whereConds.size()) {
                sc.whereConds[sq.condIdx].inList =
                    engine.subqueryValues(sq.subTable, sq.subCol,
                                          sq.subWhere, sq.subWhereLogic);
            }
        }
        if (sc.type == milansql::CommandType::SELECT) {
            milansql::Table result;
            if (!sc.whereConds.empty()) {
                auto qr = engine.selectWhere(sc.tableName, sc.whereConds, sc.whereLogic);
                result = std::move(qr.table);
            } else {
                result = engine.selectAll(sc.tableName).clone();
            }
            if (!sc.selectColumns.empty()) result = result.project(sc.selectColumns);
            if (!sc.orderByCols.empty()) result.sortByMulti(sc.orderByCols);
            dispatch_printTable(result, sc.limit, sc.limitOffset);
        } else if (sc.type == milansql::CommandType::UPDATE) {
            std::size_t n = 0;
            if (sc.whereColumn.empty())
                n = engine.updateAll(sc.tableName, sc.updateCols, sc.updateVals);
            else
                n = engine.updateWhere(sc.tableName, sc.updateCols, sc.updateVals,
                                       sc.whereColumn, sc.whereValue);
            std::cout << "  " << n << " Zeile(n) aktualisiert.\n\n";
            persistFn();
        } else if (sc.type == milansql::CommandType::INSERT) {
            const auto& rows66 = sc.multiValues.empty()
                ? std::vector<std::vector<std::string>>{sc.values} : sc.multiValues;
            for (const auto& vals : rows66) engine.insertRow(sc.tableName, vals);
            persistFn();
            std::cout << "  " << rows66.size() << " Zeile(n) eingefuegt.\n\n";
        } else if (sc.type == milansql::CommandType::DELETE) {
            std::size_t n = 0;
            if (sc.whereColumn.empty()) n = engine.deleteAll(sc.tableName);
            else n = engine.deleteWhere(sc.tableName, sc.whereColumn, sc.whereValue);
            std::cout << "  " << n << " Zeile(n) geloescht.\n\n";
            persistFn();
        } else {
            std::cout << "  [CALL] Unbekannter Befehl: '" << sql << "'\n\n";
        }
    }

    // Forward declarations — mutual recursion resolved by being in same struct
    std::string execBody(const std::string& body) {
        auto stmts = splitStmts(body);
        for (const auto& s : stmts) {
            std::string res = execStmt(s);
            if (!res.empty()) return res;
        }
        return "";
    }

    std::string execStmt(const std::string& rawStmt) {
        std::string stmt = ptrim(rawStmt);
        if (stmt.empty()) return "";
        std::string up = pu(stmt);

        // ── LEAVE label ───────────────────────────────────────────────
        if (up.size() > 6 && up.substr(0, 6) == "LEAVE ") {
            return ptrim(stmt.substr(6));
        }

        // ── DECLARE ───────────────────────────────────────────────────
        if (up.size() > 8 && up.substr(0, 8) == "DECLARE ") {
            std::vector<std::string> toks;
            { std::istringstream iss(stmt); std::string t; while (iss >> t) toks.push_back(t); }

            // DECLARE CONTINUE HANDLER FOR NOT FOUND SET var = val
            if (toks.size() >= 10 &&
                pu(toks[1]) == "CONTINUE" && pu(toks[2]) == "HANDLER" &&
                pu(toks[3]) == "FOR"      && pu(toks[4]) == "NOT"     &&
                pu(toks[5]) == "FOUND"    && pu(toks[6]) == "SET") {
                state.notFoundVar = toks[7];
                state.notFoundVal = (toks.size() > 9) ? toks[9] : "1";
                state.hasNotFoundHandler = true;
                return "";
            }
            // DECLARE name CURSOR FOR SELECT ...
            {
                size_t cfPos = up.find(" CURSOR FOR ");
                if (cfPos != std::string::npos) {
                    std::string curName = pu(ptrim(stmt.substr(8, cfPos - 8)));
                    std::string curSql  = ptrim(stmt.substr(cfPos + 12));
                    ProcState::CursorSt cs;
                    cs.sql = curSql;
                    state.cursors[curName] = cs;
                    return "";
                }
            }
            // DECLARE var TYPE [DEFAULT val]
            if (toks.size() >= 3) {
                std::string varName = toks[1];
                std::string defVal = "0";
                for (size_t k = 2; k + 1 < toks.size(); ++k) {
                    if (pu(toks[k]) == "DEFAULT") { defVal = stripQ(toks[k+1]); break; }
                }
                std::string typUp = pu(toks[2]);
                if (typUp.find("VARCHAR") != std::string::npos ||
                    typUp.find("CHAR")    != std::string::npos ||
                    typUp.find("TEXT")    != std::string::npos) {
                    if (defVal == "0") defVal = "";
                }
                state.vars[varName] = defVal;
            }
            return "";
        }

        // ── SET var = expr ────────────────────────────────────────────
        if (up.size() > 4 && up.substr(0, 4) == "SET ") {
            std::string rest = ptrim(stmt.substr(4));
            size_t eq = rest.find('=');
            if (eq != std::string::npos) {
                std::string varName = ptrim(rest.substr(0, eq));
                std::string expr    = ptrim(rest.substr(eq + 1));
                std::vector<std::string> toks;
                { std::istringstream iss(expr); std::string t; while (iss >> t) toks.push_back(t); }
                if (toks.size() == 3) {
                    const std::string& op = toks[1];
                    if (op == "+" || op == "-" || op == "*" || op == "/") {
                        try {
                            double lv = std::stod(stripQ(resolve(toks[0])));
                            double rv = std::stod(stripQ(resolve(toks[2])));
                            double res = 0;
                            if      (op == "+") res = lv + rv;
                            else if (op == "-") res = lv - rv;
                            else if (op == "*") res = lv * rv;
                            else if (op == "/" && rv != 0) res = lv / rv;
                            else { state.vars[varName] = expr; return ""; }
                            if (res == std::floor(res))
                                state.vars[varName] = std::to_string(static_cast<long long>(res));
                            else
                                state.vars[varName] = std::to_string(res);
                            return "";
                        } catch (...) {}
                    }
                }
                state.vars[varName] = stripQ(resolve(toks.empty() ? expr : toks[0]));
            }
            return "";
        }

        // ── OPEN cursor ───────────────────────────────────────────────
        if (up.size() > 5 && up.substr(0, 5) == "OPEN ") {
            std::string curName = pu(ptrim(stmt.substr(5)));
            auto it = state.cursors.find(curName);
            if (it == state.cursors.end()) {
                std::cout << "  FEHLER: Cursor '" << curName << "' nicht deklariert.\n\n";
                return "";
            }
            auto& cs = it->second;
            milansql::Parser p2;
            milansql::ParsedCommand sc = p2.parse(subst(cs.sql));
            milansql::Table result;
            if (!sc.whereConds.empty()) {
                auto qr = engine.selectWhere(sc.tableName, sc.whereConds, sc.whereLogic);
                result = std::move(qr.table);
            } else {
                result = engine.selectAll(sc.tableName).clone();
            }
            if (!sc.selectColumns.empty()) result = result.project(sc.selectColumns);
            if (!sc.orderByCols.empty()) result.sortByMulti(sc.orderByCols);
            cs.rows.clear(); cs.colNames.clear();
            for (const auto& col : result.columns()) cs.colNames.push_back(col.name);
            for (const auto& row : result.rows()) cs.rows.push_back(row.values);
            cs.pos = 0; cs.isOpen = true;
            return "";
        }

        // ── CLOSE cursor ──────────────────────────────────────────────
        if (up.size() > 6 && up.substr(0, 6) == "CLOSE ") {
            std::string curName = pu(ptrim(stmt.substr(6)));
            auto it = state.cursors.find(curName);
            if (it != state.cursors.end()) { it->second.isOpen = false; it->second.pos = 0; }
            return "";
        }

        // ── FETCH cursor INTO v1, v2 ... ──────────────────────────────
        if (up.size() > 6 && up.substr(0, 6) == "FETCH ") {
            std::string rest  = ptrim(stmt.substr(6));
            std::string upRest = pu(rest);
            size_t intoPos = upRest.find(" INTO ");
            if (intoPos == std::string::npos) {
                std::cout << "  FEHLER: FETCH ohne INTO\n\n"; return "";
            }
            std::string curName = pu(ptrim(rest.substr(0, intoPos)));
            std::string varList = ptrim(rest.substr(intoPos + 6));
            auto it = state.cursors.find(curName);
            if (it == state.cursors.end()) {
                std::cout << "  FEHLER: Cursor '" << curName << "' nicht gefunden.\n\n"; return "";
            }
            auto& cs = it->second;
            if (cs.pos >= cs.rows.size()) {
                if (state.hasNotFoundHandler && !state.notFoundVar.empty())
                    state.vars[state.notFoundVar] = state.notFoundVal;
                return "";
            }
            std::vector<std::string> varNames;
            { std::istringstream iss(varList); std::string v;
              while (std::getline(iss, v, ',')) varNames.push_back(ptrim(v)); }
            const auto& row = cs.rows[cs.pos++];
            for (size_t k = 0; k < varNames.size() && k < row.size(); ++k)
                state.vars[varNames[k]] = stripQ(row[k]);
            return "";
        }

        // ── [label:] LOOP ... END LOOP ────────────────────────────────
        {
            std::string label;
            std::string bodyPart = stmt;
            size_t colonPos = stmt.find(':');
            if (colonPos != std::string::npos && colonPos < 64) {
                std::string pl = ptrim(stmt.substr(0, colonPos));
                bool isLabel = !pl.empty();
                for (char c : pl) if (!isAlnumU(c)) { isLabel = false; break; }
                if (isLabel) { label = pl; bodyPart = ptrim(stmt.substr(colonPos + 1)); }
            }
            std::string upBody = pu(bodyPart);
            if (upBody.substr(0, 4) == "LOOP") {
                // find last END LOOP
                std::string upB = pu(bodyPart);
                size_t lastEl = std::string::npos;
                { size_t p = 0;
                  while ((p = upB.find("END LOOP", p)) != std::string::npos) { lastEl = p; p += 8; } }
                if (lastEl == std::string::npos) {
                    std::cout << "  FEHLER: LOOP ohne END LOOP\n\n"; return "";
                }
                std::string innerBody = ptrim(bodyPart.substr(4, lastEl - 4));
                for (int iter = 0; iter < 1000000; ++iter) {
                    std::string res = execBody(innerBody);
                    if (!res.empty()) {
                        if (label.empty() || pu(res) == pu(label)) return "";
                        return res;
                    }
                }
                return "";
            }
        }

        // ── IF cond THEN ... [ELSE ...] END IF ────────────────────────
        if (up.size() > 3 && up.substr(0, 3) == "IF ") {
            // Find outer THEN (skip nested IFs)
            size_t thenPos = std::string::npos;
            {
                int d = 0;
                size_t i = 3;
                while (i < up.size()) {
                    if (i + 2 <= up.size() && up.substr(i,2) == "IF" && wbLeft(up,i) && wbRight(up,i,2)) {
                        d++;
                    } else if (i + 4 <= up.size() && up.substr(i,4) == "THEN" && wbLeft(up,i) && wbRight(up,i,4)) {
                        if (d == 0) { thenPos = i; break; }
                        d--;
                    }
                    i++;
                }
            }
            if (thenPos == std::string::npos) { try { execSql(stmt); } catch (...) {} return ""; }

            std::string condStr  = ptrim(stmt.substr(3, thenPos - 3));
            std::string afterThen = ptrim(stmt.substr(thenPos + 4));
            std::string upAT = pu(afterThen);

            // Find top-level ELSE and END IF in afterThen
            size_t elsePos = std::string::npos, endIfPos = std::string::npos;
            {
                int d = 0;
                for (size_t i = 0; i < upAT.size(); ) {
                    if (i + 2 <= upAT.size() && upAT.substr(i,2) == "IF" && wbLeft(upAT,i) && wbRight(upAT,i,2)) {
                        d++; i += 2; continue;
                    }
                    if (i + 6 <= upAT.size() && upAT.substr(i,6) == "END IF" && wbLeft(upAT,i)) {
                        if (d == 0) { endIfPos = i; break; }
                        d--; i += 6; continue;
                    }
                    if (d == 0 && i + 4 <= upAT.size() && upAT.substr(i,4) == "ELSE" && wbLeft(upAT,i) && wbRight(upAT,i,4)) {
                        elsePos = i; i += 4; continue;
                    }
                    i++;
                }
            }

            std::string thenBody, elseBody;
            size_t endBound = (endIfPos != std::string::npos) ? endIfPos : afterThen.size();
            if (elsePos != std::string::npos) {
                thenBody = ptrim(afterThen.substr(0, elsePos));
                elseBody = ptrim(afterThen.substr(elsePos + 4, endBound - (elsePos + 4)));
            } else {
                thenBody = ptrim(afterThen.substr(0, endBound));
            }

            if (evalCond(condStr)) return execBody(thenBody);
            if (!elseBody.empty())  return execBody(elseBody);
            return "";
        }

        // ── Regular SQL ───────────────────────────────────────────────
        try { execSql(stmt); } catch (const std::exception& ex) {
            std::cout << "  FEHLER in Procedure: " << ex.what() << "\n\n";
        }
        return "";
    }
};

} // anonymous namespace

// ── Phase 87: Recursive CTE helpers ──────────────────────────

// Global max iterations guard (default 100, SET RECURSIVE_MAX_ITERATIONS changes it)
static int g_recursiveMaxIter = 100;

// Evaluate a single expression token against a row.
// Handles: alias-qualified names (k.id → id), bare column names, literals.
// aliasMap: user alias → actual table name (e.g. {"n":"t2", "t":"tree"})
static inline std::string dispatch_evalExprAtom(
        const std::string& tok,
        const std::vector<milansql::Column>& cols,
        const milansql::Row& row,
        const std::map<std::string,std::string>& aliasMap = {})
{
    // Parse token as "alias.col" or "col"
    std::string userAlias;
    std::string colName = tok;
    auto dot = tok.find('.');
    if (dot != std::string::npos) {
        userAlias = tok.substr(0, dot);
        colName   = tok.substr(dot + 1);
    }

    // If alias is known, resolve to actual table name first (precise match)
    if (!userAlias.empty()) {
        auto it = aliasMap.find(userAlias);
        std::string actualTbl = (it != aliasMap.end()) ? it->second : "";
        for (size_t ci = 0; ci < cols.size(); ++ci) {
            if (ci >= row.values.size()) continue;
            const std::string& cn = cols[ci].name;
            auto cdot = cn.rfind('.');
            std::string tblPart  = (cdot != std::string::npos) ? cn.substr(0, cdot) : "";
            std::string barePart = (cdot != std::string::npos) ? cn.substr(cdot + 1) : cn;
            if (barePart != colName) continue;
            // Match against resolved table name (or alias directly if not in map)
            std::string wantTbl = actualTbl.empty() ? userAlias : actualTbl;
            if (tblPart == wantTbl) return row.values[ci];
        }
        // Fallback: suffix match using resolved table name (e.g. "public.t2" ends with "t2")
        {
            std::string wantSuffix = actualTbl.empty() ? userAlias : actualTbl;
            for (size_t ci = 0; ci < cols.size(); ++ci) {
                if (ci >= row.values.size()) continue;
                const std::string& cn = cols[ci].name;
                auto cdot = cn.rfind('.');
                std::string tblPart  = (cdot != std::string::npos) ? cn.substr(0, cdot) : "";
                std::string barePart = (cdot != std::string::npos) ? cn.substr(cdot + 1) : cn;
                if (barePart != colName) continue;
                if (tblPart.empty()) return row.values[ci];
                if (tblPart.size() >= wantSuffix.size() &&
                    tblPart.substr(tblPart.size() - wantSuffix.size()) == wantSuffix)
                    return row.values[ci];
            }
        }
        // Last resort: bare column name
        for (size_t ci = 0; ci < cols.size(); ++ci) {
            if (ci >= row.values.size()) continue;
            const std::string& cn = cols[ci].name;
            auto cdot = cn.rfind('.');
            std::string bare = (cdot != std::string::npos) ? cn.substr(cdot + 1) : cn;
            if (bare == colName) return row.values[ci];
        }
        return tok;  // literal
    }

    // No alias: return first matching bare column name
    for (size_t ci = 0; ci < cols.size(); ++ci) {
        if (ci >= row.values.size()) continue;
        const std::string& cn = cols[ci].name;
        auto cdot = cn.rfind('.');
        std::string barePart = (cdot != std::string::npos) ? cn.substr(cdot + 1) : cn;
        if (barePart == colName || cn == colName) return row.values[ci];
    }

    // Not a column → treat as literal
    return tok;
}

// Evaluate a simple binary expression "left op right" or a single atom.
// Supported ops: +, -, *, /
static inline std::string dispatch_evalExprFull(
        const std::string& expr,
        const std::vector<milansql::Column>& cols,
        const milansql::Row& row,
        const std::map<std::string,std::string>& aliasMap = {})
{
    // Tokenise by whitespace
    std::vector<std::string> parts;
    {
        std::istringstream ss(expr);
        std::string t;
        while (ss >> t) parts.push_back(t);
    }

    if (parts.size() == 1)
        return dispatch_evalExprAtom(parts[0], cols, row, aliasMap);

    if (parts.size() == 3) {
        const std::string& op = parts[1];
        if (op == "+" || op == "-" || op == "*" || op == "/") {
            std::string lv = dispatch_evalExprAtom(parts[0], cols, row, aliasMap);
            std::string rv = dispatch_evalExprAtom(parts[2], cols, row, aliasMap);
            try {
                // Try integer arithmetic first
                bool lInt = true, rInt = true;
                for (char c : lv) if (c != '-' && !std::isdigit((unsigned char)c)) { lInt = false; break; }
                for (char c : rv) if (c != '-' && !std::isdigit((unsigned char)c)) { rInt = false; break; }
                if (lInt && rInt && !lv.empty() && !rv.empty()) {
                    long long l = std::stoll(lv), r = std::stoll(rv);
                    long long res = (op == "+") ? l + r
                                  : (op == "-") ? l - r
                                  : (op == "*") ? l * r
                                  : (r != 0 ? l / r : 0);
                    return std::to_string(res);
                }
                // Fall back to double arithmetic
                double ld = std::stod(lv), rd = std::stod(rv);
                double res = (op == "+") ? ld + rd
                            : (op == "-") ? ld - rd
                            : (op == "*") ? ld * rd
                            : (rd != 0.0 ? ld / rd : 0.0);
                std::ostringstream oss;
                // If result is a whole number, format without decimals
                if (res == static_cast<long long>(res))
                    oss << static_cast<long long>(res);
                else
                    oss << std::fixed << std::setprecision(6) << res;
                return oss.str();
            } catch (...) {}
        }
    }

    // Multi-token or unknown: just concatenate evaluated atoms
    std::string result;
    for (const auto& p : parts) result += dispatch_evalExprAtom(p, cols, row, aliasMap);
    return result;
}

// Custom projector: handles "expr AS alias" and alias-qualified cols.
// Builds a new Table from src with columns defined by selectCols list.
// Each entry in selectCols can be:
//   "colName"           → plain column
//   "alias.colName"     → alias-qualified column
//   "expr"              → literal or single-column reference
//   "expr AS alias"     → expression with AS alias (may span multiple space-separated tokens)
// aliasMap: user alias → actual table name, forwarded to expression evaluator
static inline milansql::Table dispatch_projectExprs(
        const milansql::Table& src,
        const std::vector<std::string>& selectCols,
        const std::map<std::string,std::string>& aliasMap = {})
{
    // Parse each selectCol into {expr, alias}
    struct ColSpec { std::string expr; std::string alias; };
    std::vector<ColSpec> specs;
    for (const auto& sc : selectCols) {
        // Find " AS " (case-insensitive)
        std::string upper = sc;
        for (char& c : upper) c = static_cast<char>(std::toupper((unsigned char)c));
        auto asPos = upper.find(" AS ");
        if (asPos != std::string::npos) {
            std::string expr  = sc.substr(0, asPos);
            std::string alias = sc.substr(asPos + 4);
            // trim
            while (!expr.empty()  && expr.front()  == ' ') expr.erase(expr.begin());
            while (!expr.empty()  && expr.back()   == ' ') expr.pop_back();
            while (!alias.empty() && alias.front() == ' ') alias.erase(alias.begin());
            while (!alias.empty() && alias.back()  == ' ') alias.pop_back();
            specs.push_back({expr, alias});
        } else {
            // No AS alias: use last component after dot as display name
            std::string nm = sc;
            auto dot = nm.rfind('.');
            if (dot != std::string::npos) nm = nm.substr(dot + 1);
            specs.push_back({sc, nm});
        }
    }

    // Build result schema
    std::vector<milansql::Column> outCols;
    for (const auto& sp : specs)
        outCols.emplace_back(sp.alias, "TEXT");
    milansql::Table result("__proj", outCols);

    const auto& srcCols = src.columns();
    for (const auto& row : src.rows()) {
        std::vector<std::string> vals;
        for (const auto& sp : specs)
            vals.push_back(dispatch_evalExprFull(sp.expr, srcCols, row, aliasMap));
        result.insert(milansql::Row(std::move(vals)));
    }
    return result;
}

// Execute a literal SELECT (no FROM) — e.g. "SELECT 1 AS n, 0 AS a, 1 AS b"
// Returns a single-row Table with the given literal columns.
static inline bool dispatch_executeLiteralSelect(
        const std::string& sql,
        milansql::Table& result)
{
    // Parse: after SELECT, split by comma. Each item: "expr [AS alias]"
    std::string up = sql;
    for (char& c : up) c = static_cast<char>(std::toupper((unsigned char)c));
    auto selPos = up.find("SELECT");
    if (selPos == std::string::npos) return false;
    // Check no FROM
    auto fromPos = up.find(" FROM ");
    if (fromPos != std::string::npos) return false;

    std::string colList = sql.substr(selPos + 6);
    while (!colList.empty() && colList.front() == ' ') colList.erase(colList.begin());

    // Split by top-level commas
    std::vector<std::string> items;
    {
        int depth = 0;
        std::string cur;
        for (char c : colList) {
            if (c == '(') ++depth;
            else if (c == ')') --depth;
            else if (c == ',' && depth == 0) { items.push_back(cur); cur.clear(); continue; }
            cur += c;
        }
        if (!cur.empty()) items.push_back(cur);
    }

    std::vector<milansql::Column> cols;
    std::vector<std::string> vals;
    for (auto& item : items) {
        // trim
        while (!item.empty() && item.front() == ' ') item.erase(item.begin());
        while (!item.empty() && item.back()  == ' ') item.pop_back();
        std::string upper = item;
        for (char& c : upper) c = static_cast<char>(std::toupper((unsigned char)c));
        auto asPos = upper.find(" AS ");
        std::string expr, alias;
        if (asPos != std::string::npos) {
            expr  = item.substr(0, asPos);
            alias = item.substr(asPos + 4);
            while (!expr.empty()  && expr.front()  == ' ') expr.erase(expr.begin());
            while (!expr.empty()  && expr.back()   == ' ') expr.pop_back();
            while (!alias.empty() && alias.front() == ' ') alias.erase(alias.begin());
            while (!alias.empty() && alias.back()  == ' ') alias.pop_back();
        } else {
            expr = item; alias = item;
        }
        cols.emplace_back(alias, "TEXT");
        vals.push_back(expr);  // literal value
    }

    result = milansql::Table("__literal", cols);
    result.insert(milansql::Row(vals));
    return true;
}

// Split "anchorSQL UNION ALL recursiveSQL" at top-level UNION ALL.
// Returns {anchorSql, recursiveSql} or {"",""} if not found.
static inline std::pair<std::string,std::string>
dispatch_splitUnionAll(const std::string& sql)
{
    // Tokenise and find top-level UNION ALL
    std::vector<std::string> toks;
    {
        std::istringstream ss(sql);
        std::string t;
        while (ss >> t) toks.push_back(t);
    }
    int depth = 0;
    for (size_t i = 0; i + 1 < toks.size(); ++i) {
        std::string up0 = toks[i], up1 = toks[i+1];
        for (char& c : up0) c = static_cast<char>(std::toupper((unsigned char)c));
        for (char& c : up1) c = static_cast<char>(std::toupper((unsigned char)c));
        if (toks[i] == "(") ++depth;
        else if (toks[i] == ")") --depth;
        if (depth == 0 && up0 == "UNION" && up1 == "ALL") {
            // Reconstruct left and right
            std::string left, right;
            for (size_t j = 0; j < i; ++j) {
                if (j > 0) left += " ";
                left += toks[j];
            }
            for (size_t j = i + 2; j < toks.size(); ++j) {
                if (j > i + 2) right += " ";
                right += toks[j];
            }
            return {left, right};
        }
    }
    return {"", ""};
}

// Check if a selectColumns list has any expression (AS alias, arithmetic, alias.col)
static inline bool dispatch_hasExprColumns(const std::vector<std::string>& cols) {
    for (const auto& sc : cols) {
        std::string up = sc;
        for (char& c : up) c = static_cast<char>(std::toupper((unsigned char)c));
        if (up.find(" AS ") != std::string::npos ||
            up.find('+') != std::string::npos ||
            up.find('-') != std::string::npos ||
            up.find('*') != std::string::npos ||
            up.find('/') != std::string::npos ||
            up.find('.') != std::string::npos)
            return true;
    }
    return false;
}

// Execute a SELECT command that may have expression columns.
// Falls back to dispatch_projectExprs when project() would fail.
static inline milansql::Table dispatch_execSelectWithExprs(
        milansql::Engine& engine,
        milansql::Parser& parser,
        milansql::ParsedCommand& cmd,
        const std::vector<milansql::Column>& targetSchema)
{
    bool hasExpr = dispatch_hasExprColumns(cmd.selectColumns);

    if (!hasExpr) {
        // Safe to use standard path
        return dispatch_executeSelectToTable(engine, parser, cmd);
    }

    // Expression path: get raw rows (no projection), then apply dispatch_projectExprs
    milansql::Table raw;
    auto selColsSaved = cmd.selectColumns;
    cmd.selectColumns.clear();  // suppress project() in standard path

    // Build alias map: user alias → actual table name (Phase 87)
    std::map<std::string,std::string> aliasMap;
    if (!cmd.tableAlias.empty()) aliasMap[cmd.tableAlias] = cmd.tableName;
    for (const auto& jc : cmd.joinClauses)
        if (!jc.tableAlias.empty()) aliasMap[jc.tableAlias] = jc.table;

    if (cmd.isJoin) {
        milansql::QueryOptimizer qopt;
        qopt.optimize(cmd, engine);

        // Resolve user aliases in ON conditions before calling executeJoins
        auto joinClauses = cmd.joinClauses;
        for (auto& jc : joinClauses) {
            auto replAlias = [&](std::string& side) {
                auto dot = side.find('.');
                if (dot == std::string::npos) return;
                std::string tbl = side.substr(0, dot);
                std::string col = side.substr(dot + 1);
                auto it = aliasMap.find(tbl);
                if (it != aliasMap.end()) side = it->second + "." + col;
            };
            replAlias(jc.onLeft);
            replAlias(jc.onRight);
        }
        raw = engine.executeJoins(cmd.tableName, joinClauses,
                                  cmd.whereConds, cmd.whereLogic);
    } else if (!cmd.whereConds.empty()) {
        auto qr = engine.selectWhere(cmd.tableName, cmd.whereConds, cmd.whereLogic);
        raw = std::move(qr.table);
    } else {
        raw = engine.selectAll(cmd.tableName).clone();
    }
    cmd.selectColumns = selColsSaved;

    milansql::Table projected = dispatch_projectExprs(raw, cmd.selectColumns, aliasMap);

    // Rename columns to match target schema if provided
    if (!targetSchema.empty() && projected.columns().size() == targetSchema.size()) {
        milansql::Table renamed("__cte_expr", targetSchema);
        for (const auto& r : projected.rows()) renamed.insert(r);
        return renamed;
    }
    return projected;
}

// Execute a recursive CTE and return the final accumulated table.
// cteName: name of the CTE (registered as temp table)
// innerSql: full body "anchorSQL UNION ALL recursiveSQL"
static inline milansql::Table dispatch_executeRecursiveCTE(
        milansql::Engine& engine,
        milansql::Parser& parser,
        const std::string& cteName,
        const std::string& innerSql,
        int maxIter)
{
    auto [anchorSql, recursiveSql] = dispatch_splitUnionAll(innerSql);
    if (anchorSql.empty())
        throw std::runtime_error("WITH RECURSIVE: kein UNION ALL in '" + cteName + "'");

    // ── Step 1: Evaluate anchor ───────────────────────────────
    milansql::Table anchor;
    milansql::ParsedCommand anchorCmd = parser.parse(anchorSql);

    if (anchorCmd.type == milansql::CommandType::UNKNOWN) {
        // Literal SELECT (no FROM): e.g. SELECT 1 AS n, 0 AS a, 1 AS b
        if (!dispatch_executeLiteralSelect(anchorSql, anchor))
            throw std::runtime_error("WITH RECURSIVE: Anchor konnte nicht ausgewertet werden: " + anchorSql);
    } else {
        // Regular SELECT (possibly with expression columns)
        anchor = dispatch_execSelectWithExprs(engine, parser, anchorCmd, {});
    }

    // Build output schema from anchor's column names
    std::vector<milansql::Column> anchorCols = anchor.columns();

    // ── Step 2: Accumulate ────────────────────────────────────
    milansql::Table accumulated("__cte_" + cteName, anchorCols);
    for (const auto& r : anchor.rows())
        accumulated.insert(r);

    // ── Step 3: Iterate ───────────────────────────────────────
    milansql::Table working = anchor.clone();  // seed = anchor

    for (int iter = 0; iter < maxIter; ++iter) {
        // Register current working set as the CTE temp table
        engine.registerTempTable(cteName, working.clone());

        // Parse and execute recursive part against current working set
        milansql::ParsedCommand recCmd = parser.parse(recursiveSql);

        milansql::Table newRows;
        if (recCmd.type == milansql::CommandType::UNKNOWN) {
            // Literal SELECT (rare in recursive part)
            if (!dispatch_executeLiteralSelect(recursiveSql, newRows)) break;
        } else {
            newRows = dispatch_execSelectWithExprs(engine, parser, recCmd, anchorCols);
        }

        if (newRows.rowCount() == 0) break;

        // Append new rows to accumulated result
        for (const auto& r : newRows.rows())
            accumulated.insert(r);

        // New working set = only newly produced rows
        working = milansql::Table("__cte_" + cteName, anchorCols);
        for (const auto& r : newRows.rows())
            working.insert(r);
    }

    // Register final result as the CTE table for the main query
    engine.registerTempTable(cteName, accumulated.clone());
    return accumulated;
}

// ── dispatchCommand ───────────────────────────────────────────
// Main dispatch function: takes a parsed command and executes it against the engine.
// Writes output to std::cout (can be redirected via rdbuf for capture).
// Returns true if EXIT command was received.
inline bool dispatchCommand(
    milansql::ParsedCommand& cmd,
    milansql::Engine& engine,
    milansql::Parser& parser,
    const std::string& eingabe,
    std::function<void()> persistFn,
    std::function<void()> saveProceduresFn,
    std::function<void()> saveTriggFn,
    std::function<std::string()> getProcessListFn = nullptr)
{
    // Phase 82: record query statistics + apply rewrites before dispatch
    g_adaptiveStats.recordQuery(cmd);
    if (cmd.type == milansql::CommandType::SELECT)
        g_queryRewriter.rewrite(cmd);

    switch (cmd.type) {

    case milansql::CommandType::EXIT:
        std::cout << "Auf Wiedersehen!\n";
        return true;

    case milansql::CommandType::HELP: {
        std::cout << "\n"
            << "  Verfuegbare Befehle:\n"
            << "  \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\n"
            << "  CREATE TABLE name (col TYP [NOT NULL] [UNIQUE] [DEFAULT val]\n"
            << "  SELECT * FROM name                        Alle Zeilen\n"
            << "  INSERT INTO name VALUES (...)             Zeile einfuegen\n"
            << "  UPDATE name SET col=val [WHERE ...]       Zeilen aendern\n"
            << "  DELETE FROM name [WHERE ...]              Zeilen loeschen\n"
            << "  SHOW TABLES / DESCRIBE name / STATUS      Info-Befehle\n"
            << "  help / exit\n"
            << "  \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\n\n";
        break;
    }

    case milansql::CommandType::SHOW_TABLES: {
        auto namen = engine.getAllTableNames();
        auto views = engine.getAllViewNames();
        if (namen.empty() && views.empty()) {
            std::cout << "  (Keine Tabellen oder Views vorhanden)\n\n";
            break;
        }
        struct RowInfo { std::string name, typ, cols, rows; };
        std::vector<RowInfo> infos;
        for (const auto& n : namen) {
            const auto& t = engine.selectAll(n);
            infos.push_back({n, "TABLE",
                std::to_string(t.columns().size()),
                std::to_string(t.rowCount())});
        }
        for (const auto& v : views) {
            infos.push_back({v, "VIEW", "-", "-"});
        }
        std::vector<std::string> hdr = {"Name", "Typ", "Spalten", "Zeilen"};
        std::vector<size_t> w = {hdr[0].size(), hdr[1].size(), hdr[2].size(), hdr[3].size()};
        for (const auto& r : infos) {
            w[0] = std::max(w[0], r.name.size());
            w[1] = std::max(w[1], r.typ.size());
            w[2] = std::max(w[2], r.cols.size());
            w[3] = std::max(w[3], r.rows.size());
        }
        auto stHline = [&](const std::string& l, const std::string& m,
                           const std::string& r2, const std::string& f) {
            std::cout << l;
            for (size_t i = 0; i < 4; ++i) {
                for (size_t j = 0; j < w[i] + 2; ++j) std::cout << f;
                if (i + 1 < 4) std::cout << m;
            }
            std::cout << r2 << "\n";
        };
        auto stCell = [](const std::string& s, size_t width) {
            std::cout << " " << s;
            for (size_t j = s.size(); j < width; ++j) std::cout << " ";
            std::cout << " \u2502";
        };
        std::cout << "\n";
        stHline("\u250c", "\u252c", "\u2510", "\u2500");
        std::cout << "\u2502";
        for (size_t i = 0; i < 4; ++i) stCell(hdr[i], w[i]);
        std::cout << "\n";
        stHline("\u251c", "\u253c", "\u2524", "\u2500");
        for (const auto& r : infos) {
            std::cout << "\u2502";
            stCell(r.name, w[0]);
            stCell(r.typ,  w[1]);
            stCell(r.cols, w[2]);
            stCell(r.rows, w[3]);
            std::cout << "\n";
        }
        stHline("\u2514", "\u2534", "\u2518", "\u2500");
        std::cout << "  " << infos.size() << " Eintrag/Eintraege\n\n";
        break;
    }

    case milansql::CommandType::SHOW_INDEXES: {
        if (cmd.tableName.empty()) {
            std::cout << "  Fehler: SHOW INDEXES FROM tabellenname\n"; break;
        }
        std::cout << "\n";
        // Combine B-Tree indexes and FULLTEXT indexes
        auto allIndexes = engine.getIndexes(cmd.tableName);
        auto ftIndexes  = engine.getFulltextIndexes(cmd.tableName);
        for (const auto& fi : ftIndexes) allIndexes.push_back(fi);
        dispatch_printIndexes(allIndexes, cmd.tableName);
        break;
    }

    case milansql::CommandType::DESCRIBE: {
        if (cmd.tableName.empty()) {
            std::cout << "  Fehler: DESCRIBE tabellenname\n"; break;
        }
        if (engine.viewExists(cmd.tableName)) {
            std::cout << "\n  View: " << cmd.tableName << "\n"
                      << "  SQL:  " << engine.getViewSql(cmd.tableName) << "\n\n";
        } else {
            dispatch_printDescribe(engine.selectAll(cmd.tableName));
        }
        break;
    }

    // ── Phase 80: CREATE COLUMN TABLE ────────────────────────────
    case milansql::CommandType::CREATE_COLUMN_TABLE: {
        if (cmd.tableName.empty() || cmd.columns.empty()) {
            std::cout << "  Fehler: CREATE COLUMN TABLE name (col TYP, ...)\n"; break;
        }
        engine.createColumnTable(cmd.tableName, cmd.columns);
        std::cout << "  Column Table '" << cmd.tableName << "' erstellt ("
                  << cmd.columns.size() << " Spalten, COLUMN STORE).\n\n";
        break;
    }

    // ── Phase 84: CREATE PAGED TABLE ─────────────────────────
    case milansql::CommandType::CREATE_PAGED_TABLE: {
        if (cmd.tableName.empty() || cmd.columns.empty()) {
            std::cout << "  Fehler: CREATE PAGED TABLE name (col TYP, ...)\n"; break;
        }
        engine.createPagedTable(cmd.tableName, cmd.columns);
        std::cout << "  Paged Table '" << cmd.tableName << "' erstellt ("
                  << cmd.columns.size() << " Spalten, PAGE STORE, 8KB Pages).\n\n";
        break;
    }

    // ── Phase 84: SHOW PAGE STATS ─────────────────────────────
    case milansql::CommandType::SHOW_PAGE_STATS: {
        engine.showPageStats();
        break;
    }

    // ── Phase 84: FLUSH PAGES ─────────────────────────────────
    case milansql::CommandType::FLUSH_PAGES: {
        engine.flushPages();
        break;
    }

    // ── Phase 84: SET USE_PAGED_STORAGE ──────────────────────
    case milansql::CommandType::SET_USE_PAGED_STORAGE: {
        std::string val = cmd.values.empty() ? "" : cmd.values[0];
        if (val == "ON")
            std::cout << "  USE_PAGED_STORAGE = ON (neue Tabellen als Paged Tables anlegen)\n\n";
        else if (val == "OFF")
            std::cout << "  USE_PAGED_STORAGE = OFF (neue Tabellen als Row Store anlegen)\n\n";
        else
            std::cout << "  Fehler: SET USE_PAGED_STORAGE = ON | OFF\n\n";
        break;
    }

    case milansql::CommandType::CREATE_TABLE: {
        if (dispatch_slaveReadOnly()) {
            std::cout << "  FEHLER: Read-only: Slave akzeptiert keine Schreiboperationen\n\n";
            break;
        }
        if (cmd.tableName.empty() || cmd.columns.empty()) {
            std::cout << "  Fehler: CREATE TABLE name (col TYP, ...)\n"; break;
        }
        engine.createTable(cmd.tableName, cmd.columns, cmd.foreignKeys,
                           cmd.tableInherits); // Phase 78: INHERITS
        // Auto-create BTREE index for PRIMARY KEY column
        for (const auto& col : cmd.columns) {
            if (col.isPrimaryKey) {
                try {
                    engine.createIndex(cmd.tableName, {col.name}, "PRIMARY");
                } catch (...) {}
                break;
            }
        }
        // Phase 62: Apply partition info if specified
        if (!cmd.partitionType.empty()) {
            milansql::PartitionInfo pi;
            pi.column = cmd.partitionColumn;
            if (cmd.partitionType == "RANGE") {
                pi.type = milansql::PartitionType::RANGE;
                for (auto& r : cmd.partitionRanges) {
                    milansql::PartitionRangeDef rd;
                    rd.name = r.name;
                    rd.limitStr = r.limitStr;
                    if (r.limitStr == "MAXVALUE")
                        rd.limit = std::numeric_limits<long long>::max();
                    else { try { rd.limit = std::stoll(r.limitStr); } catch (...) { rd.limit = 0; } }
                    pi.ranges.push_back(rd);
                }
            } else if (cmd.partitionType == "LIST") {
                pi.type = milansql::PartitionType::LIST;
                for (auto& l : cmd.partitionLists) {
                    milansql::PartitionListDef ld;
                    ld.name = l.name;
                    ld.values = l.values;
                    pi.lists.push_back(ld);
                }
            } else if (cmd.partitionType == "HASH") {
                pi.type = milansql::PartitionType::HASH;
                pi.hashCount = cmd.partitionHashCount;
            }
            try { engine.setTablePartitionInfo(cmd.tableName, pi); } catch (...) {}
            dispatch_savePartitions(engine);
        }
        persistFn();
        dispatch_binlogWrite(eingabe);
        engine.invalidateCache(cmd.tableName);
        // Phase 78: if INHERITS, invalidate parent table cache too
        if (!cmd.tableInherits.empty())
            engine.invalidateCache(cmd.tableInherits);
        std::cout << "  Tabelle '" << cmd.tableName << "' erstellt ("
                  << cmd.columns.size() << " Spalten";
        if (!cmd.foreignKeys.empty())
            std::cout << ", " << cmd.foreignKeys.size() << " FK";
        if (!cmd.partitionType.empty())
            std::cout << ", PARTITION BY " << cmd.partitionType;
        std::cout << ").\n\n";
        break;
    }

    case milansql::CommandType::INSERT: {
        if (dispatch_slaveReadOnly()) {
            std::cout << "  FEHLER: Read-only: Slave akzeptiert keine Schreiboperationen\n\n";
            break;
        }
        if (cmd.tableName.empty()) {
            std::cout << "  Fehler: Kein Tabellenname.\n"; break;
        }
        if (milansql::Engine::isInfoSchemaName(cmd.tableName)) {
            std::cout << "  FEHLER: INFORMATION_SCHEMA ist read-only.\n\n"; break;
        }
        // Phase 84: route to paged table
        if (engine.isPagedTable(cmd.tableName)) {
            std::vector<std::vector<std::string>> rows;
            if (cmd.multiValues.empty())
                rows.push_back(cmd.values);
            else
                rows = cmd.multiValues;
            for (const auto& vals : rows)
                engine.insertIntoPagedTable(cmd.tableName, vals);
            if (rows.size() == 1)
                std::cout << "  1 Zeile eingefuegt in '" << cmd.tableName << "' (PAGE STORE).\n\n";
            else
                std::cout << "  " << rows.size() << " Zeilen eingefuegt in '"
                          << cmd.tableName << "' (PAGE STORE).\n\n";
            break;
        }
        // Phase 80: route to column store
        if (engine.isColumnTable(cmd.tableName)) {
            auto& ct = engine.getColumnTable(cmd.tableName);
            std::vector<std::vector<std::string>> colRows;
            if (cmd.multiValues.empty())
                colRows.push_back(cmd.values);
            else
                colRows = cmd.multiValues;
            for (const auto& vals : colRows)
                ct.insert(vals);
            if (colRows.size() == 1)
                std::cout << "  1 Zeile eingefuegt in '" << cmd.tableName << "' (COLUMN STORE).\n\n";
            else
                std::cout << "  " << colRows.size() << " Zeilen eingefuegt in '"
                          << cmd.tableName << "' (COLUMN STORE).\n\n";
            break;
        }
        if (cmd.isInsertSelect) {
            if (cmd.insertSelectSql.empty()) {
                std::cout << "  Fehler: INSERT INTO name SELECT ...\n"; break;
            }
            milansql::ParsedCommand sc = parser.parse(cmd.insertSelectSql);
            for (const auto& sq : sc.subqueries) {
                if (sq.condIdx < sc.whereConds.size()) {
                    sc.whereConds[sq.condIdx].inList =
                        engine.subqueryValues(sq.subTable, sq.subCol,
                                              sq.subWhere, sq.subWhereLogic);
                }
            }
            milansql::Table result;
            if (!sc.whereConds.empty()) {
                auto qr = engine.selectWhere(sc.tableName, sc.whereConds, sc.whereLogic);
                result = std::move(qr.table);
            } else {
                result = engine.selectAll(sc.tableName).clone();
            }
            if (!sc.selectColumns.empty())
                result = result.project(sc.selectColumns);
            size_t count = 0;
            for (const auto& row : result.rows()) {
                engine.insertRow(cmd.tableName, row.values);
                ++count;
            }
            persistFn();
            dispatch_binlogWrite(eingabe);
            engine.invalidateCache(cmd.tableName);
            std::cout << "  " << count << " Zeile(n) eingefuegt in '"
                      << cmd.tableName << "' (INSERT INTO SELECT).\n\n";
            break;
        }

        // Phase 55: Werte bei benannter Spalten-Liste neu anordnen
        // INSERT INTO t (col2, col4) VALUES (v2, v4) → [NULL, v2, NULL, v4]
        auto remapForNamedCols =
            [&](std::vector<std::string> srcVals) -> std::vector<std::string> {
            if (cmd.insertColumnNames.empty()) return srcVals;
            try {
                const auto& tblCols = engine.tableColumns(cmd.tableName);
                // "" = Sentinel: "nicht angegeben" → applyDefaults füllt DEFAULT ein
                std::vector<std::string> out(tblCols.size(), "");
                for (size_t si = 0; si < cmd.insertColumnNames.size() && si < srcVals.size(); ++si) {
                    std::string cn = cmd.insertColumnNames[si];
                    for (char& c : cn)
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    for (size_t ti = 0; ti < tblCols.size(); ++ti) {
                        std::string tn = tblCols[ti].name;
                        for (char& c : tn)
                            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if (tn == cn) { out[ti] = srcVals[si]; break; }
                    }
                }
                return out;
            } catch (...) { return srcVals; }
        };

        std::vector<std::vector<std::string>> rowsStorage;
        if (cmd.multiValues.empty())
            rowsStorage.push_back(remapForNamedCols(cmd.values));
        else
            for (const auto& v : cmd.multiValues)
                rowsStorage.push_back(remapForNamedCols(v));
        const auto& rows = rowsStorage;

        if (cmd.upsertMode == "REPLACE") {
            size_t replaced = 0, inserted = 0;
            for (const auto& vals : rows)
                engine.insertOrReplace(cmd.tableName, vals) ? ++replaced : ++inserted;
            persistFn();
            dispatch_binlogWrite(eingabe);
            if (replaced > 0 && inserted > 0)
                std::cout << "  " << replaced << " Zeile(n) ersetzt, "
                          << inserted << " eingefuegt in '"
                          << cmd.tableName << "'.\n\n";
            else if (replaced > 0)
                std::cout << "  " << replaced << " Zeile(n) ersetzt in '"
                          << cmd.tableName << "'.\n\n";
            else
                std::cout << "  " << inserted << " Zeile(n) eingefuegt in '"
                          << cmd.tableName << "'.\n\n";
        } else if (cmd.upsertMode == "IGNORE") {
            size_t inserted = 0, ignored = 0;
            for (const auto& vals : rows)
                engine.insertOrIgnore(cmd.tableName, vals) ? ++inserted : ++ignored;
            persistFn();
            dispatch_binlogWrite(eingabe);
            if (ignored > 0 && inserted > 0)
                std::cout << "  " << inserted << " Zeile(n) eingefuegt, "
                          << ignored << " ignoriert (Konflikt) in '"
                          << cmd.tableName << "'.\n\n";
            else if (ignored > 0)
                std::cout << "  " << ignored << " Zeile(n) ignoriert (Konflikt) in '"
                          << cmd.tableName << "'.\n\n";
            else
                std::cout << "  " << inserted << " Zeile(n) eingefuegt in '"
                          << cmd.tableName << "'.\n\n";
        } else {
            for (const auto& vals : rows)
                engine.insertRow(cmd.tableName, vals);
            persistFn();
            dispatch_binlogWrite(eingabe);
            engine.invalidateCache(cmd.tableName);
            if (rows.size() == 1)
                std::cout << "  1 Zeile eingefuegt in '" << cmd.tableName << "'.\n\n";
            else
                std::cout << "  " << rows.size() << " Zeilen eingefuegt in '"
                          << cmd.tableName << "'.\n\n";
        }
        break;
    }

    case milansql::CommandType::SELECT: {
        // ── Phase 60: SELECT … INTO OUTFILE ──────────────────────
        // If csvFile is set (stripped from "INTO OUTFILE" suffix by parser),
        // execute the SELECT via dispatch_executeSelectToTable and write CSV.
        if (!cmd.csvFile.empty() && cmd.cteList.empty()) {
            try {
                char sep = milansql::CsvUtils::parseSepChar(
                    cmd.csvSeparator.empty() ? "," : cmd.csvSeparator);
                milansql::Table result =
                    dispatch_executeSelectToTable(engine, parser, cmd);
                // Build header + rows
                std::vector<std::string> headers;
                for (const auto& c : result.columns())
                    headers.push_back(c.name);
                std::vector<std::vector<std::string>> csvRows;
                for (const auto& row : result.rows()) {
                    std::vector<std::string> csvRow;
                    for (size_t ci = 0; ci < result.columns().size(); ++ci)
                        csvRow.push_back(dispatch_displayVal(
                            ci < row.values.size() ? row.values[ci] : ""));
                    csvRows.push_back(std::move(csvRow));
                }
                milansql::CsvUtils::writeFile(cmd.csvFile, headers, csvRows, sep);
                std::cout << "  " << csvRows.size()
                          << " Zeile(n) exportiert nach '"
                          << cmd.csvFile << "' (Separator: '"
                          << (sep == '\t' ? "\\t" : std::string(1, sep))
                          << "').\n\n";
            } catch (const std::exception& ex) {
                std::cout << "  FEHLER (INTO OUTFILE): " << ex.what() << "\n\n";
            }
            break;
        }

        // Phase 55: SELECT ohne FROM (z.B. SELECT NOW(), SELECT 1+1)
        if (cmd.tableName.empty() && cmd.hasCaseItems && !cmd.selectItems.empty()) {
            std::vector<milansql::Column> resCols;
            std::vector<std::string>      resVals;
            for (const auto& item : cmd.selectItems) {
                // Spaltenüberschrift
                std::string header = item.alias;
                if (header.empty() && item.isFuncExpr) {
                    header = item.funcName + "(";
                    for (size_t ai = 0; ai < item.funcArgs.size(); ++ai) {
                        if (ai) header += ",";
                        header += item.funcArgs[ai];
                    }
                    header += ")";
                }
                if (header.empty()) header = item.colName;
                resCols.push_back(milansql::Column(header, "TEXT"));

                // Wert auswerten
                std::string val;
                if (item.isFuncExpr) {
                    val = engine.evalFuncPublic(item.funcName, item.funcArgs);
                } else {
                    val = item.colName;
                }
                resVals.push_back(val);
            }
            milansql::Table result("", resCols);
            result.insert(milansql::Row(std::move(resVals)));
            std::cout << "\n";
            dispatch_printTable(result, -1, 0);
            break;
        }
        if (cmd.tableName.empty()) {
            std::cout << "  Fehler: Kein Tabellenname.\n"; break;
        }
        if (!engine.viewExists(cmd.tableName) && cmd.cteList.empty()) {
            engine.checkPrivilege("SELECT", cmd.tableName);
        }

        if (!cmd.cteList.empty()) {
            try {
                if (cmd.isRecursiveCte) {
                    // ── Phase 87: WITH RECURSIVE ──────────────────
                    for (auto& [cteName, cteInnerSql] : cmd.cteList) {
                        dispatch_executeRecursiveCTE(engine, parser,
                            cteName, cteInnerSql, g_recursiveMaxIter);
                        // result is already registered as temp table
                    }
                } else {
                    // ── Phase 41: WITH (non-recursive) ────────────
                    for (auto& [cteName, cteInnerSql] : cmd.cteList) {
                        milansql::ParsedCommand cteParsed = parser.parse(cteInnerSql);
                        milansql::Table cteResult =
                            dispatch_executeSelectToTable(engine, parser, cteParsed);
                        engine.registerTempTable(cteName, std::move(cteResult));
                    }
                }
                milansql::Table mainResult =
                    dispatch_executeSelectToTable(engine, parser, cmd);
                engine.cleanupTempTables();
                std::cout << "\n";
                if (!cmd.orderByCols.empty())
                    mainResult.sortByMulti(cmd.orderByCols);
                dispatch_printTable(mainResult, cmd.limit, cmd.limitOffset);
            } catch (...) {
                engine.cleanupTempTables();
                throw;
            }
            break;
        }

        // ── Phase 54B: EXPLAIN ANALYZE ────────────────────────────
        if (cmd.isExplainAnalyze) {
            using clk = std::chrono::high_resolution_clock;
            using fms = std::chrono::duration<double, std::milli>;
            struct AStep {
                int step; std::string op, tbl, detail;
                size_t rows; double ms;
            };
            std::vector<AStep> steps;
            int sno = 1;
            double totalMs = 0.0;

            // Step 1: SCAN / FILTER
            auto t0 = clk::now();
            milansql::Table result;
            bool usedIdx = false;
            if (!cmd.whereConds.empty()) {
                auto qr = engine.selectWhere(cmd.tableName, cmd.whereConds, cmd.whereLogic);
                usedIdx = qr.usedIndex;
                result  = std::move(qr.table);
            } else {
                result = engine.selectAllFiltered(cmd.tableName); // Phase 75: RLS
            }
            auto t1 = clk::now();
            double scanMs = fms(t1 - t0).count();
            totalMs += scanMs;
            std::string scanOp  = usedIdx ? "INDEX SCAN" : "SCAN";
            std::string scanDet = cmd.whereConds.empty() ? "full table" : "WHERE " + dispatch_whereDesc(cmd);
            steps.push_back({sno++, scanOp, cmd.tableName, scanDet, result.rowCount(), scanMs});

            // Step 2: SORT (if ORDER BY)
            if (!cmd.orderByCols.empty()) {
                auto t2 = clk::now();
                result.sortByMulti(cmd.orderByCols);
                auto t3 = clk::now();
                double ms2 = fms(t3 - t2).count();
                totalMs += ms2;
                steps.push_back({sno++, "SORT", "-", "ORDER BY", result.rowCount(), ms2});
            }

            // Step 3: PROJECT
            {
                auto t4 = clk::now();
                if (cmd.hasCaseItems && !cmd.selectItems.empty()) {
                    bool hasWin = false;
                    for (const auto& si : cmd.selectItems)
                        if (si.isWindowFunc) { hasWin = true; break; }
                    result = hasWin
                        ? engine.projectWithWindowItems(result, cmd.selectItems)
                        : engine.projectWithItems(result, cmd.selectItems);
                } else if (!cmd.selectColumns.empty()) {
                    result = result.project(cmd.selectColumns);
                }
                if (cmd.isDistinct) result.makeDistinct();
                auto t5 = clk::now();
                double ms3 = fms(t5 - t4).count();
                totalMs += ms3;
                std::string projDet = cmd.selectColumns.empty() ? "*"
                    : std::to_string(cmd.selectColumns.size()) + " Spalten";
                steps.push_back({sno++, "PROJECT", "-", projDet, result.rowCount(), ms3});
            }

            // Print analyze table
            auto fmtMs = [](double v) {
                std::ostringstream ss;
                ss << std::fixed << std::setprecision(3) << v << "ms";
                return ss.str();
            };
            struct Col { std::string h; size_t w; };
            std::vector<Col> hdr = {{"Schritt",8},{"Operation",10},{"Tabelle",12},{"Details",20},{"Zeilen",7},{"Zeit",9}};
            for (const auto& s : steps) {
                hdr[0].w = std::max(hdr[0].w, std::to_string(s.step).size());
                hdr[1].w = std::max(hdr[1].w, s.op.size());
                hdr[2].w = std::max(hdr[2].w, s.tbl.size());
                hdr[3].w = std::max(hdr[3].w, s.detail.size());
                hdr[4].w = std::max(hdr[4].w, std::to_string(s.rows).size());
                hdr[5].w = std::max(hdr[5].w, fmtMs(s.ms).size());
            }
            hdr[5].w = std::max(hdr[5].w, fmtMs(totalMs).size());
            auto printCols = [&](const std::vector<std::string>& vals) {
                std::cout << "\u2502";
                for (size_t i = 0; i < hdr.size(); ++i) {
                    std::cout << " " << vals[i];
                    for (size_t j = vals[i].size(); j < hdr[i].w; ++j) std::cout << " ";
                    std::cout << " \u2502";
                }
                std::cout << "\n";
            };
            auto hlineA = [&](const std::string& l, const std::string& m, const std::string& r) {
                std::cout << l;
                for (size_t i = 0; i < hdr.size(); ++i) {
                    for (size_t j = 0; j < hdr[i].w + 2; ++j) std::cout << "\u2500";
                    if (i + 1 < hdr.size()) std::cout << m;
                }
                std::cout << r << "\n";
            };
            std::cout << "\n";
            hlineA("\u250c", "\u252c", "\u2510");
            std::vector<std::string> hdrvec;
            for (const auto& c : hdr) hdrvec.push_back(c.h);
            printCols(hdrvec);
            hlineA("\u251c", "\u253c", "\u2524");
            for (const auto& s : steps) {
                printCols({std::to_string(s.step), s.op, s.tbl, s.detail,
                           std::to_string(s.rows), fmtMs(s.ms)});
            }
            hlineA("\u251c", "\u253c", "\u2524");
            printCols({"TOTAL", "", "", "", std::to_string(result.rowCount()), fmtMs(totalMs)});
            hlineA("\u2514", "\u2534", "\u2518");
            std::cout << "\n";
            // Also print actual result
            dispatch_printTable(result, cmd.limit, cmd.limitOffset);
            break;
        }

        if (cmd.isExplain) {
            // Phase 48: Run optimizer on a copy of cmd to collect notes
            milansql::ParsedCommand cmdOpt = cmd;
            milansql::QueryOptimizer qopt;
            auto optNotes = qopt.optimize(cmdOpt, engine);

            // Build ExplainRequest from (possibly reordered) cmdOpt
            milansql::ExplainRequest req;
            req.tableName     = cmdOpt.tableName;
            req.isJoin        = cmdOpt.isJoin;
            req.joinClauses   = cmdOpt.joinClauses;
            req.whereConds    = cmdOpt.whereConds;
            req.whereLogic    = cmdOpt.whereLogic;
            req.isGroupBy     = cmdOpt.isGroupBy;
            req.groupByCols   = cmdOpt.groupByCols;
            req.havingConds   = cmdOpt.havingConds;
            req.isAggregate   = cmdOpt.isAggregate;
            req.aggFunc       = cmdOpt.aggFunc;
            req.aggCol        = cmdOpt.aggCol;
            req.selectItems   = cmdOpt.selectItems;
            req.hasCaseItems  = cmdOpt.hasCaseItems;
            req.selectColumns = cmdOpt.selectColumns;
            req.orderByCols   = cmdOpt.orderByCols;
            req.limit         = cmdOpt.limit;
            req.limitOffset   = cmdOpt.limitOffset;
            req.isSetOp       = cmdOpt.isSetOp;
            req.setOp         = cmdOpt.setOp;

            // Convert OptimizationNote to Engine::OptimizerNote
            std::vector<milansql::Engine::OptimizerNote> engineNotes;
            for (const auto& n : optNotes) {
                milansql::Engine::OptimizerNote en;
                en.step       = n.step;
                en.original   = n.original;
                en.optimized  = n.optimized;
                en.costBefore = n.costBefore;
                en.costAfter  = n.costAfter;
                engineNotes.push_back(en);
            }

            auto plan = engine.buildExplainWithNotes(req, engineNotes);
            // Phase 62: Add partition pruning info to EXPLAIN
            if (!cmd.tableName.empty() && !cmd.whereConds.empty()) {
                try {
                    const auto& pi = engine.getTablePartitionInfo(cmd.tableName);
                    if (pi.hasPartitions()) {
                        for (auto& wc : cmd.whereConds) {
                            if (wc.col == pi.column && !wc.val.empty()) {
                                auto pruned = engine.prunePartitions(
                                    cmd.tableName, wc.col, wc.op, wc.val);
                                std::string partStr;
                                for (size_t pi2 = 0; pi2 < pruned.size(); ++pi2) {
                                    if (pi2) partStr += ", ";
                                    partStr += pruned[pi2];
                                }
                                int nr = static_cast<int>(plan.steps.size()) + 1;
                                plan.steps.push_back({nr, "PARTITION PRUNING",
                                    cmd.tableName,
                                    "Partitions: " + partStr, "-"});
                                break;
                            }
                        }
                    }
                } catch (...) {}
            }
            dispatch_printExplain(plan);
            // Phase 86: Show statistics-based cardinality estimate if available
            if (!cmd.tableName.empty() && g_tableStats.hasStats(cmd.tableName)) {
                size_t est = g_tableStats.estimateRowCount(cmd.tableName, cmd.whereConds);
                if (!cmd.whereConds.empty()) {
                    double sel = 1.0;
                    for (const auto& wc : cmd.whereConds)
                        sel *= g_tableStats.estimateSelectivity(
                            cmd.tableName, wc.col, wc.op, wc.val);
                    std::cout << "  [Statistik] Geschaetzte Zeilen: " << est
                              << "  Selektivitaet: "
                              << std::fixed << std::setprecision(4) << sel << "\n\n";
                }
            }
            break;
        }

        if (cmd.isSetOp) {
            if (cmd.rightSql.empty()) {
                std::cout << "  Fehler: " << cmd.setOp << " braucht rechte SELECT-Seite.\n\n";
                break;
            }
            milansql::Table leftResult;
            if (!cmd.whereConds.empty()) {
                auto qr = engine.selectWhere(cmd.tableName, cmd.whereConds, cmd.whereLogic);
                leftResult = std::move(qr.table);
            } else {
                leftResult = engine.selectAll(cmd.tableName).clone();
            }
            if (!cmd.selectColumns.empty())
                leftResult = leftResult.project(cmd.selectColumns);

            milansql::ParsedCommand rc = parser.parse(cmd.rightSql);
            for (const auto& sq : rc.subqueries) {
                if (sq.condIdx < rc.whereConds.size())
                    rc.whereConds[sq.condIdx].inList =
                        engine.subqueryValues(sq.subTable, sq.subCol,
                                              sq.subWhere, sq.subWhereLogic);
            }
            milansql::Table rightResult;
            if (!rc.whereConds.empty()) {
                auto qr = engine.selectWhere(rc.tableName, rc.whereConds, rc.whereLogic);
                rightResult = std::move(qr.table);
            } else {
                rightResult = engine.selectAll(rc.tableName).clone();
            }
            if (!rc.selectColumns.empty())
                rightResult = rightResult.project(rc.selectColumns);

            milansql::Table result =
                engine.executeSetOp(leftResult, cmd.setOp, rightResult);
            std::cout << "\n";
            if (!cmd.orderByCols.empty()) result.sortByMulti(cmd.orderByCols);
            dispatch_printTable(result, cmd.limit, cmd.limitOffset);
            break;
        }

        // ── Phase 72: Materialized View — use cached data ────────
        if (engine.isMaterializedView(cmd.tableName)) {
            const auto& mv = engine.getMaterializedView(cmd.tableName);
            milansql::Table result(cmd.tableName, mv.columns);
            for (const auto& row : mv.data)
                result.insert(milansql::Row(row));
            if (!cmd.whereConds.empty())
                result = engine.filterTable(result, cmd.whereConds, cmd.whereLogic);
            std::cout << "\n";
            if (!cmd.orderByCols.empty())
                result.sortByMulti(cmd.orderByCols);
            if (!cmd.selectColumns.empty())
                result = result.project(cmd.selectColumns);
            if (cmd.isDistinct)
                result.makeDistinct();
            dispatch_printTable(result, cmd.limit, cmd.limitOffset);
            break;
        }

        if (engine.viewExists(cmd.tableName)) {
            milansql::Table result = dispatch_materializeView(engine, parser, cmd.tableName, cmd);
            std::cout << "\n";
            if (!cmd.orderByCols.empty())
                result.sortByMulti(cmd.orderByCols);
            if (!cmd.selectColumns.empty())
                result = result.project(cmd.selectColumns);
            if (cmd.isDistinct)
                result.makeDistinct();
            dispatch_printTable(result, cmd.limit, cmd.limitOffset);
            break;
        }

        // Phase 84: Paged Table — full scan
        if (engine.isPagedTable(cmd.tableName)) {
            if (cmd.isCount) {
                size_t n = engine.countPagedTable(cmd.tableName);
                std::cout << "\n  COUNT(*) = " << n
                          << " (Page Store '" << cmd.tableName << "')";
                if (!cmd.whereConds.empty())
                    std::cout << "  [WHERE " << dispatch_whereDesc(cmd) << "]";
                std::cout << "\n\n";
                break;
            }
            milansql::Table result = engine.selectFromPagedTable(cmd.tableName);
            if (!cmd.whereConds.empty())
                result = engine.filterTable(result, cmd.whereConds, cmd.whereLogic);
            std::cout << "\n";
            if (!cmd.orderByCols.empty())
                result.sortByMulti(cmd.orderByCols);
            if (!cmd.selectColumns.empty())
                result = result.project(cmd.selectColumns);
            if (cmd.isDistinct)
                result.makeDistinct();
            dispatch_printTable(result, cmd.limit, cmd.limitOffset);
            break;
        }

        // Phase 80: Column Store — route aggregates + full scans
        if (engine.isColumnTable(cmd.tableName)) {
            const auto& ct = engine.getColumnTableConst(cmd.tableName);
            if (cmd.isAggregate) {
                std::string val;
                std::string func = cmd.aggFunc;
                for (char& c : func) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (func == "COUNT")
                    val = std::to_string(ct.countWhere(cmd.whereConds, cmd.whereLogic));
                else if (func == "SUM")
                    val = ct.aggregateSumWhere(cmd.aggCol, cmd.whereConds, cmd.whereLogic);
                else if (func == "AVG")
                    val = ct.aggregateAvgWhere(cmd.aggCol, cmd.whereConds, cmd.whereLogic);
                else if (func == "MIN")
                    val = ct.aggregateMinWhere(cmd.aggCol, cmd.whereConds, cmd.whereLogic);
                else if (func == "MAX")
                    val = ct.aggregateMaxWhere(cmd.aggCol, cmd.whereConds, cmd.whereLogic);
                else
                    val = "NULL";
                std::cout << "\n  " << cmd.aggFunc << "(" << cmd.aggCol << ") = "
                          << val << " (Column Store '" << cmd.tableName << "')";
                if (!cmd.whereConds.empty())
                    std::cout << "  [WHERE " << dispatch_whereDesc(cmd) << "]";
                std::cout << "\n\n";
                break;
            }
            if (cmd.isCount) {
                size_t n = ct.countWhere(cmd.whereConds, cmd.whereLogic);
                std::cout << "\n  COUNT(*) = " << n
                          << " (Column Store '" << cmd.tableName << "')";
                if (!cmd.whereConds.empty())
                    std::cout << "  [WHERE " << dispatch_whereDesc(cmd) << "]";
                std::cout << "\n\n";
                break;
            }
            if (cmd.isGroupBy) {
                // Fall back to row-store conversion for GROUP BY
                milansql::Table rowTable = cmd.whereConds.empty()
                    ? ct.toRows()
                    : ct.filterRows(cmd.whereConds, cmd.whereLogic);
                engine.registerTempTable("__cs_tmp__", std::move(rowTable));
                try {
                    auto result = engine.groupBy(
                        "__cs_tmp__", {}, "AND",
                        cmd.groupByCols, cmd.selectItems,
                        cmd.havingConds, cmd.havingLogic);
                    engine.cleanupTempTables();
                    std::cout << "\n";
                    if (!cmd.orderByCols.empty())
                        result.sortByMulti(cmd.orderByCols);
                    dispatch_printTable(result, cmd.limit, cmd.limitOffset);
                } catch (...) {
                    engine.cleanupTempTables();
                    throw;
                }
                break;
            }
            // Full scan / WHERE
            milansql::Table result = cmd.whereConds.empty()
                ? ct.toRows()
                : ct.filterRows(cmd.whereConds, cmd.whereLogic);
            std::cout << "\n";
            if (!cmd.orderByCols.empty())
                result.sortByMulti(cmd.orderByCols);
            if (!cmd.selectColumns.empty())
                result = result.project(cmd.selectColumns);
            dispatch_printTable(result, cmd.limit, cmd.limitOffset);
            break;
        }

        if (cmd.isJoin) {
            if (cmd.joinClauses.empty()) {
                std::cout << "  Fehler: JOIN-Syntax: "
                             "FROM t1 [LEFT|INNER] JOIN t2 ON t1.col = t2.col\n";
                break;
            }
            // Phase 89: Register any foreign tables as temp tables before join
            {
                std::vector<std::string> fdwToClean;
                auto registerFdwMain = [&](const std::string& tbl) {
                    if (engine.isForeignTable(tbl)) {
                        milansql::Table ft = engine.executeForeignScan(tbl);
                        engine.registerTempTable(tbl, std::move(ft));
                        fdwToClean.push_back(tbl);
                    }
                };
                registerFdwMain(cmd.tableName);
                for (const auto& jc : cmd.joinClauses) registerFdwMain(jc.table);

                // Phase 48: silently optimize join order before execution
                {
                    milansql::QueryOptimizer qopt;
                    qopt.optimize(cmd, engine);
                }
                auto result = engine.executeJoins(
                    cmd.tableName, cmd.joinClauses,
                    cmd.whereConds, cmd.whereLogic);
                for (const auto& tbl : fdwToClean)
                    engine.dropTempTable(tbl);
                std::cout << "\n";
                if (!cmd.orderByCols.empty())
                    result.sortByMulti(cmd.orderByCols);
                if (!cmd.selectColumns.empty())
                    result = result.project(cmd.selectColumns);
                dispatch_printTable(result, cmd.limit, cmd.limitOffset);
            }
            break;
        }

        if (cmd.isGroupBy) {
            if (cmd.groupByCols.empty()) {
                std::cout << "  Fehler: GROUP BY ohne Spalten.\n"; break;
            }
            auto result = engine.groupBy(
                cmd.tableName,
                cmd.whereConds, cmd.whereLogic,
                cmd.groupByCols,
                cmd.selectItems,
                cmd.havingConds, cmd.havingLogic);
            std::cout << "\n";
            if (!cmd.orderByCols.empty())
                result.sortByMulti(cmd.orderByCols);
            dispatch_printTable(result, cmd.limit, cmd.limitOffset);
            break;
        }

        if (cmd.isAggregate) {
            std::string val = engine.computeAggregate(
                cmd.tableName, cmd.aggFunc, cmd.aggCol,
                cmd.whereConds, cmd.whereLogic);
            std::cout << "\n  " << cmd.aggFunc << "(" << cmd.aggCol << ") = "
                      << val << " (Tabelle '" << cmd.tableName << "')";
            if (!cmd.whereConds.empty())
                std::cout << "  [WHERE " << dispatch_whereDesc(cmd) << "]";
            std::cout << "\n\n";
            break;
        }

        if (cmd.isCount) {
            std::size_t n = engine.countWhere(
                cmd.tableName, cmd.whereConds, cmd.whereLogic, cmd.fromOnly); // Phase 78: ONLY
            std::cout << "\n  COUNT(*) = " << n
                      << " (Tabelle '" << cmd.tableName << "')";
            if (!cmd.whereConds.empty())
                std::cout << "  [WHERE " << dispatch_whereDesc(cmd) << "]";
            std::cout << "\n\n";
            break;
        }

        // ── Phase 54A: Query Cache check ──────────────────────────
        {
            auto& qc = engine.getQueryCache();
            if (qc.isEnabled()) {
                // Phase 75: include current user in cache key so RLS results
                // are not shared across different users
                std::string cacheKey = engine.getCurrentUser() + "\x01" + eingabe;
                auto cached = qc.get(cacheKey);
                if (cached) {
                    std::cout << *cached << "  [CACHE HIT]\n\n";
                    break;
                }
            }
        }

        // Execute and optionally capture for caching
        {
            // Phase 69: Profiler start
            if (g_profiler.isEnabled()) g_profiler.startQuery(eingabe);

            auto& qc = engine.getQueryCache();
            // Redirect cout to ostringstream when cache is active
            std::ostringstream captureStream;
            std::streambuf* oldBuf = nullptr;
            if (qc.isEnabled()) {
                oldBuf = std::cout.rdbuf(captureStream.rdbuf());
            }

            // Phase 69: Optimization step
            if (g_profiler.isEnabled()) g_profiler.addStep("Optimization");

            std::cout << "\n";
            milansql::Table result;
            bool usedIndex = false;

            // Phase 89: Foreign Data Wrapper — intercept SELECT on foreign tables
            if (engine.isForeignTable(cmd.tableName)) {
                milansql::Table foreignResult = engine.executeForeignScan(cmd.tableName);
                if (!cmd.whereConds.empty())
                    foreignResult = engine.filterTable(foreignResult, cmd.whereConds, cmd.whereLogic);
                if (!cmd.orderByCols.empty()) foreignResult.sortByMulti(cmd.orderByCols);
                if (!cmd.selectColumns.empty())
                    foreignResult = foreignResult.project(cmd.selectColumns);
                if (cmd.isDistinct) foreignResult.makeDistinct();
                dispatch_printTable(foreignResult, cmd.limit, cmd.limitOffset);
                if (oldBuf) {
                    std::cout.rdbuf(oldBuf);
                    std::string captured = captureStream.str();
                    std::string cacheKey = engine.getCurrentUser() + "\x01" + eingabe;
                    qc.put(cacheKey, captured, cmd.tableName);
                    std::cout << captured;
                }
                if (g_profiler.isEnabled()) g_profiler.endQuery();
                break;
            }

            // Phase 77: apply /*+ PARALLEL(N) */ hint
            int p77_savedWorkers = 0;
            if (cmd.parallelHint > 0) {
                p77_savedWorkers = engine.getMaxParallelWorkers();
                engine.setMaxParallelWorkers(cmd.parallelHint);
            }

            if (!cmd.whereConds.empty()) {
                // Phase 69: Table scan step
                if (g_profiler.isEnabled()) g_profiler.addStep("Table scan");
                auto qr = engine.selectWhere(
                    cmd.tableName, cmd.whereConds, cmd.whereLogic, cmd.fromOnly); // Phase 78: ONLY
                usedIndex = qr.usedIndex;
                result    = std::move(qr.table);
                if (g_profiler.isEnabled()) g_profiler.addStep("Result filtering");
            } else {
                if (g_profiler.isEnabled()) g_profiler.addStep("Table scan");
                result = engine.selectAllFiltered(cmd.tableName, cmd.fromOnly); // Phase 75/78: RLS+ONLY
                if (g_profiler.isEnabled()) g_profiler.addStep("Result filtering");
            }
            // Phase 77: restore hint workers
            if (cmd.parallelHint > 0)
                engine.setMaxParallelWorkers(p77_savedWorkers);

            // Phase 65: SELECT FOR UPDATE — acquire row-level WRITE locks
            if (cmd.isForUpdate) {
                try {
                    engine.acquireForUpdateLocks(cmd.tableName, result);
                } catch (const std::exception& ex) {
                    if (oldBuf) std::cout.rdbuf(oldBuf);
                    if (g_profiler.isEnabled()) g_profiler.endQuery();
                    std::cout << "  FEHLER (FOR UPDATE): " << ex.what() << "\n\n";
                    break;
                }
            }

            std::size_t totalFound = result.rowCount();

            if (totalFound == 0 && !cmd.whereConds.empty()) {
                std::string lbl = usedIndex ? "[INDEX SCAN]" : "[FULL SCAN] ";
                std::cout << "  " << lbl << " 0 Zeilen gefunden (WHERE "
                          << dispatch_whereDesc(cmd) << ")\n\n";
                if (oldBuf) { std::cout.rdbuf(oldBuf); std::cout << captureStream.str(); }
                if (g_profiler.isEnabled()) g_profiler.endQuery();
                break;
            }

            if (cmd.hasCaseItems && !cmd.selectItems.empty()) {
                if (g_profiler.isEnabled()) g_profiler.addStep("Result projection");
                bool hasWin = false;
                bool hasUnnest88 = false;
                for (const auto& si : cmd.selectItems) {
                    if (si.isWindowFunc) { hasWin = true; }
                    if (si.isUnnest) { hasUnnest88 = true; }
                }
                if (hasWin)
                    result = engine.projectWithWindowItems(result, cmd.selectItems);
                else
                    result = engine.projectWithItems(result, cmd.selectItems);
                // Phase 88: expand UNNEST after projection
                if (hasUnnest88)
                    result = dispatch_expandUnnestResult(result, cmd.selectItems);
                if (!cmd.orderByCols.empty()) {
                    if (g_profiler.isEnabled()) g_profiler.addStep("Sorting");
                    result.sortByMulti(cmd.orderByCols);
                }
            } else {
                if (!cmd.orderByCols.empty()) {
                    if (g_profiler.isEnabled()) g_profiler.addStep("Sorting");
                    result.sortByMulti(cmd.orderByCols);
                }
                if (!cmd.selectColumns.empty()) {
                    if (g_profiler.isEnabled()) g_profiler.addStep("Result projection");
                    result = result.project(cmd.selectColumns);
                }
            }
            if (cmd.isDistinct)
                result.makeDistinct();

            dispatch_printTable(result, cmd.limit, cmd.limitOffset);

            if (!cmd.whereConds.empty()) {
                std::string lbl = usedIndex ? "[INDEX SCAN]" : "[FULL SCAN] ";
                std::cout << "  " << lbl << " " << totalFound
                          << " Zeile(n) (WHERE " << dispatch_whereDesc(cmd) << ")\n\n";
            }

            if (oldBuf) {
                std::cout.rdbuf(oldBuf);
                std::string captured = captureStream.str();
                // Phase 75: user-scoped cache key for RLS correctness
                std::string cacheKey = engine.getCurrentUser() + "\x01" + eingabe;
                qc.put(cacheKey, captured, cmd.tableName);
                std::cout << captured;
            }

            // Phase 69: Profiler end
            if (g_profiler.isEnabled()) g_profiler.endQuery();
        }
        break;
    }

    case milansql::CommandType::CREATE_VIEW: {
        if (cmd.tableName.empty() || cmd.viewSql.empty()) {
            std::cout << "  Fehler: CREATE VIEW name AS SELECT ...\n"; break;
        }
        engine.createView(cmd.tableName, cmd.viewSql);
        persistFn();
        std::cout << "  View '" << cmd.tableName << "' erstellt.\n\n";
        break;
    }

    case milansql::CommandType::DROP_VIEW: {
        if (cmd.tableName.empty()) {
            std::cout << "  Fehler: DROP VIEW viewname\n"; break;
        }
        engine.dropView(cmd.tableName);
        persistFn();
        std::cout << "  View '" << cmd.tableName << "' geloescht.\n\n";
        break;
    }

    case milansql::CommandType::CREATE_INDEX: {
        if (cmd.tableName.empty() || cmd.indexColumns.empty()) {
            std::cout << "  Fehler: CREATE INDEX name ON tabelle (spalte [, ...])\n"; break;
        }
        engine.createIndex(cmd.tableName, cmd.indexColumns, cmd.indexName);
        std::string colList;
        for (size_t i = 0; i < cmd.indexColumns.size(); ++i) {
            if (i > 0) colList += ", ";
            colList += cmd.indexColumns[i];
        }
        std::cout << "  Index '" << cmd.indexName << "' auf "
                  << cmd.tableName << "(" << colList << ") erstellt"
                  << " [B-Tree T=" << milansql::BTree::T << "].\n\n";
        break;
    }

    case milansql::CommandType::DROP_INDEX: {
        if (cmd.tableName.empty() || cmd.indexName.empty()) {
            std::cout << "  Fehler: DROP INDEX name ON tabelle\n"; break;
        }
        engine.dropIndex(cmd.tableName, cmd.indexName);
        std::cout << "  Index '" << cmd.indexName
                  << "' von '" << cmd.tableName << "' geloescht.\n\n";
        break;
    }

    case milansql::CommandType::UPDATE: {
        if (dispatch_slaveReadOnly()) {
            std::cout << "  FEHLER: Read-only: Slave akzeptiert keine Schreiboperationen\n\n";
            break;
        }
        if (milansql::Engine::isInfoSchemaName(cmd.tableName)) {
            std::cout << "  FEHLER: INFORMATION_SCHEMA ist read-only.\n\n"; break;
        }
        if (cmd.tableName.empty() || cmd.updateCols.empty()) {
            std::cout << "  Fehler: UPDATE tabelle SET col=val [, col=val ...] [WHERE ...]\n"; break;
        }
        std::string setDesc;
        for (size_t k = 0; k < cmd.updateCols.size(); ++k) {
            if (k > 0) setDesc += ", ";
            setDesc += cmd.updateCols[k] + "=" + cmd.updateVals[k];
        }
        if (cmd.whereColumn.empty()) {
            std::size_t n = engine.updateAll(
                cmd.tableName, cmd.updateCols, cmd.updateVals);
            persistFn();
            dispatch_binlogWrite(eingabe);
            engine.invalidateCache(cmd.tableName);
            std::cout << "  " << n << " Zeile(n) aktualisiert"
                      << " (SET " << setDesc << ")\n\n";
        } else {
            std::size_t n = engine.updateWhere(
                cmd.tableName,
                cmd.updateCols, cmd.updateVals,
                cmd.whereColumn, cmd.whereValue);
            if (n > 0) {
                persistFn();
                dispatch_binlogWrite(eingabe);
                engine.invalidateCache(cmd.tableName);
            }
            std::cout << "  " << n << " Zeile(n) aktualisiert"
                      << " (SET " << setDesc
                      << " WHERE " << cmd.whereColumn
                      << " = " << cmd.whereValue << ")\n\n";
        }
        break;
    }

    case milansql::CommandType::TRUNCATE: {
        if (dispatch_slaveReadOnly()) {
            std::cout << "  FEHLER: Read-only: Slave akzeptiert keine Schreiboperationen\n\n";
            break;
        }
        if (cmd.tableName.empty()) {
            std::cout << "  Fehler: TRUNCATE TABLE tabellenname\n"; break;
        }
        engine.truncateTable(cmd.tableName);
        persistFn();
        dispatch_binlogWrite(eingabe);
        engine.invalidateCache(cmd.tableName);
        std::cout << "  Tabelle '" << cmd.tableName
                  << "' geleert (Schema + Constraints behalten,"
                  << " AUTO_INCREMENT = 1).\n\n";
        break;
    }

    case milansql::CommandType::DELETE: {
        if (dispatch_slaveReadOnly()) {
            std::cout << "  FEHLER: Read-only: Slave akzeptiert keine Schreiboperationen\n\n";
            break;
        }
        if (milansql::Engine::isInfoSchemaName(cmd.tableName)) {
            std::cout << "  FEHLER: INFORMATION_SCHEMA ist read-only.\n\n"; break;
        }
        if (cmd.tableName.empty()) {
            std::cout << "  Fehler: DELETE FROM tabelle [WHERE ...]\n"; break;
        }
        if (cmd.whereColumn.empty()) {
            std::size_t n = engine.deleteAll(cmd.tableName);
            persistFn();
            dispatch_binlogWrite(eingabe);
            engine.invalidateCache(cmd.tableName);
            std::cout << "  " << n << " Zeile(n) geloescht.\n\n";
        } else {
            std::size_t n = engine.deleteWhere(
                cmd.tableName, cmd.whereColumn, cmd.whereValue);
            if (n > 0) {
                persistFn();
                dispatch_binlogWrite(eingabe);
                engine.invalidateCache(cmd.tableName);
            }
            std::cout << "  " << n << " Zeile(n) geloescht"
                      << " (WHERE " << cmd.whereColumn
                      << " = " << cmd.whereValue << ")\n\n";
        }
        break;
    }

    case milansql::CommandType::STATUS: {
        auto tnames = engine.getAllTableNames();
        auto vnames = engine.getAllViewNames();
        size_t totalRows = 0;
        size_t totalCols = 0;
        for (const auto& n : tnames) {
            const auto& t = engine.selectAll(n);
            totalRows += t.rowCount();
            totalCols += t.columns().size();
        }
        std::string fileSize = "?";
        {
            std::ifstream fs("database.milan", std::ios::binary | std::ios::ate);
            if (fs) fileSize = std::to_string(fs.tellg()) + " Bytes";
            else    fileSize = "(noch nicht gespeichert)";
        }
        // Phase 58: Pool-Statistiken
        long long totalReq = milansql::g_poolStats.totalRequests.load();
        long long totalUs  = milansql::g_poolStats.totalQueryTimeUs.load();
        double avgMs = (totalReq > 0)
            ? (static_cast<double>(totalUs) / static_cast<double>(totalReq) / 1000.0)
            : 0.0;
        std::ostringstream avgStr;
        avgStr << std::fixed << std::setprecision(3) << avgMs << "ms";

        struct KV { std::string key, val; };
        std::vector<KV> kvs = {
            {"Version",          "MilanSQL v2.0.0"},
            {"Datei",            "database.milan"},
            {"Format-Version",   std::to_string(milansql::MilanBinaryStorage::FORMAT_VERSION)},
            {"Tabellen",         std::to_string(tnames.size())},
            {"Views",            std::to_string(vnames.size())},
            {"Gesamt-Spalten",   std::to_string(totalCols)},
            {"Gesamt-Zeilen",    std::to_string(totalRows)},
            {"Datei-Groesse",    fileSize},
            {"---",              ""},   // Trennlinie
            {"Pool Size",        std::to_string(milansql::g_poolStats.poolSize.load())},
            {"Active Workers",   std::to_string(milansql::g_poolStats.activeWorkers.load())},
            {"Queued Requests",  std::to_string(milansql::g_poolStats.queuedRequests.load())},
            {"Total Requests",   std::to_string(totalReq)},
            {"Avg Query Time",   avgStr.str()},
        };
        size_t maxKey = 0, maxVal = 0;
        for (const auto& kv : kvs) {
            if (kv.key == "---") continue;  // Trennlinie überspringen
            maxKey = std::max(maxKey, kv.key.size());
            maxVal = std::max(maxVal, kv.val.size());
        }
        size_t boxW = maxKey + maxVal + 7;
        auto hline = [&](const std::string& l, const std::string& r) {
            std::cout << "  " << l;
            for (size_t i = 0; i < boxW; ++i) std::cout << "\u2500";
            std::cout << r << "\n";
        };
        std::string title = "MilanSQL \u2014 Datenbank-Status";
        size_t pad = (boxW > title.size() + 2) ? (boxW - title.size()) / 2 : 1;
        std::cout << "\n";
        hline("\u250c", "\u2510");
        std::cout << "  \u2502";
        for (size_t i = 0; i < pad; ++i) std::cout << " ";
        std::cout << title;
        size_t after = boxW - pad - title.size();
        for (size_t i = 0; i < after; ++i) std::cout << " ";
        std::cout << "\u2502\n";
        hline("\u251c", "\u2524");
        for (const auto& kv : kvs) {
            if (kv.key == "---") {
                // Trennlinie innerhalb der Box
                std::cout << "  \u251c";
                for (size_t i = 0; i < boxW; ++i) std::cout << "\u2500";
                std::cout << "\u2524\n";
                continue;
            }
            std::cout << "  \u2502  " << kv.key;
            for (size_t i = kv.key.size(); i < maxKey; ++i) std::cout << " ";
            std::cout << " : " << kv.val;
            for (size_t i = kv.val.size(); i < maxVal; ++i) std::cout << " ";
            std::cout << "  \u2502\n";
        }
        hline("\u2514", "\u2518");
        std::cout << "\n";
        break;
    }

    // ── Phase 54A: Query Cache commands ──────────────────────────
    case milansql::CommandType::SHOW_CACHE:
        engine.getQueryCache().showStats();
        break;

    case milansql::CommandType::CLEAR_CACHE:
        engine.getQueryCache().clear();
        std::cout << "  Query Cache geleert.\n\n";
        break;

    case milansql::CommandType::SET_CACHE:
        if (cmd.cacheEnabled == "ON") {
            engine.getQueryCache().setEnabled(true);
            std::cout << "  Query Cache aktiviert.\n\n";
        } else if (cmd.cacheEnabled == "OFF") {
            engine.getQueryCache().setEnabled(false);
            std::cout << "  Query Cache deaktiviert.\n\n";
        } else {
            std::cout << "  Fehler: SET CACHE ON oder SET CACHE OFF\n";
        }
        break;

    // ── Phase 54D: SHOW PROCESSLIST ───────────────────────────
    case milansql::CommandType::SHOW_PROCESSLIST: {
        if (getProcessListFn) {
            std::cout << getProcessListFn();
        } else {
            std::cout << "\n  SHOW PROCESSLIST ist nur im Server-Modus verfuegbar.\n\n";
        }
        break;
    }

    case milansql::CommandType::SHOW_CREATE_TABLE: {
        if (cmd.tableName.empty()) {
            std::cout << "  Fehler: SHOW CREATE TABLE tabellenname\n"; break;
        }
        if (engine.viewExists(cmd.tableName)) {
            std::cout << "\n  CREATE VIEW " << cmd.tableName
                      << " AS " << engine.getViewSql(cmd.tableName) << "\n\n";
        } else {
            const auto& tbl = engine.selectAll(cmd.tableName);
            std::cout << "\n  " << dispatch_buildCreateTableSql(tbl) << "\n\n";
        }
        break;
    }

    case milansql::CommandType::DROP_TABLE: {
        if (dispatch_slaveReadOnly()) {
            std::cout << "  FEHLER: Read-only: Slave akzeptiert keine Schreiboperationen\n\n";
            break;
        }
        if (cmd.tableName.empty()) {
            std::cout << "  Fehler: Kein Tabellenname.\n"; break;
        }
        try {
            engine.dropTable(cmd.tableName);
            persistFn();
            dispatch_binlogWrite(eingabe);
            std::cout << "  Tabelle '" << cmd.tableName << "' geloescht.\n\n";
        } catch (const std::exception& ex) {
            if (cmd.ifExists)
                std::cout << "  (Tabelle '" << cmd.tableName << "' nicht vorhanden, uebersprungen)\n\n";
            else
                throw;
        }
        break;
    }

    case milansql::CommandType::ALTER_TABLE: {
        if (dispatch_slaveReadOnly()) {
            std::cout << "  FEHLER: Read-only: Slave akzeptiert keine Schreiboperationen\n\n";
            break;
        }
        if (cmd.tableName.empty() || cmd.alterOp.empty()) {
            std::cout << "  Fehler: ALTER TABLE name ADD|DROP COLUMN/PARTITION ...\n";
            break;
        }
        // Phase 62: ADD PARTITION / DROP PARTITION
        if (cmd.alterOp == "ADD_PARTITION") {
            try {
                // Determine if RANGE or LIST based on which def was filled
                if (!cmd.addRangeDef.name.empty()) {
                    milansql::PartitionRangeDef rd;
                    rd.name = cmd.addRangeDef.name;
                    rd.limitStr = cmd.addRangeDef.limitStr;
                    if (rd.limitStr == "MAXVALUE")
                        rd.limit = std::numeric_limits<long long>::max();
                    else { try { rd.limit = std::stoll(rd.limitStr); } catch (...) { rd.limit = 0; } }
                    engine.addRangePartition(cmd.tableName, rd);
                    dispatch_savePartitions(engine);
                    std::cout << "  Partition '" << rd.name << "' zu '"
                              << cmd.tableName << "' hinzugefuegt.\n\n";
                } else if (!cmd.addListDef.name.empty()) {
                    milansql::PartitionListDef ld;
                    ld.name = cmd.addListDef.name;
                    ld.values = cmd.addListDef.values;
                    engine.addListPartition(cmd.tableName, ld);
                    dispatch_savePartitions(engine);
                    std::cout << "  Partition '" << ld.name << "' zu '"
                              << cmd.tableName << "' hinzugefuegt.\n\n";
                } else {
                    std::cout << "  Fehler: Keine gueltige Partition-Definition.\n\n";
                }
            } catch (const std::exception& e) {
                std::cout << "  Fehler: " << e.what() << "\n\n";
            }
            break;
        }
        if (cmd.alterOp == "DROP_PARTITION") {
            try {
                engine.dropPartitionByName(cmd.tableName, cmd.partitionName);
                dispatch_savePartitions(engine);
                std::cout << "  Partition '" << cmd.partitionName << "' aus '"
                          << cmd.tableName << "' geloescht.\n\n";
            } catch (const std::exception& e) {
                std::cout << "  Fehler: " << e.what() << "\n\n";
            }
            break;
        }
        // Phase 75: RLS enable/disable via ALTER TABLE
        if (cmd.alterOp == "ENABLE_RLS") {
            engine.enableRls(cmd.tableName);
            std::cout << "  RLS enabled on " << cmd.tableName << ".\n\n";
            break;
        } else if (cmd.alterOp == "DISABLE_RLS") {
            engine.disableRls(cmd.tableName);
            std::cout << "  RLS disabled on " << cmd.tableName << ".\n\n";
            break;
        }
        engine.alterTable(cmd.tableName, cmd.alterOp,
                          cmd.alterColName, cmd.alterColType,
                          cmd.alterColNew);
        persistFn();
        dispatch_binlogWrite(eingabe);
        if (cmd.alterOp == "ADD")
            std::cout << "  Spalte '" << cmd.alterColName
                      << "' (" << cmd.alterColType << ") zu '"
                      << cmd.tableName << "' hinzugefuegt.\n\n";
        else if (cmd.alterOp == "DROP")
            std::cout << "  Spalte '" << cmd.alterColName
                      << "' aus '" << cmd.tableName << "' entfernt.\n\n";
        else if (cmd.alterOp == "RENAME")
            std::cout << "  Spalte '" << cmd.alterColName
                      << "' umbenannt zu '" << cmd.alterColNew
                      << "' in '" << cmd.tableName << "'.\n\n";
        break;
    }

    // Phase 62: SHOW PARTITIONS FROM table
    case milansql::CommandType::SHOW_PARTITIONS: {
        try {
            auto lines = engine.showPartitions(cmd.tableName);
            std::cout << "\n";
            for (auto& l : lines) std::cout << "  " << l << "\n";
            std::cout << "\n";
        } catch (const std::exception& e) {
            std::cout << "  Fehler: " << e.what() << "\n\n";
        }
        break;
    }

    case milansql::CommandType::BEGIN:
        engine.beginTransaction("database.milan.wal");
        std::cout << "  Transaktion gestartet.\n\n";
        break;

    case milansql::CommandType::COMMIT:
        engine.applyAndCommit();
        engine.getQueryCache().clear();  // Phase 71: invalidate cache after tx commit
        persistFn();
        engine.deleteWal();             // Phase 72: delete WAL only after successful persist
        std::cout << "  Transaktion erfolgreich abgeschlossen (COMMIT).\n\n";
        // Phase 85: auto-checkpoint if interval reached
        if (engine.shouldAutoCheckpoint()) {
            engine.doCheckpoint();
        }
        break;

    case milansql::CommandType::ROLLBACK:
        engine.rollbackTransaction();
        std::cout << "  Transaktion abgebrochen (ROLLBACK).\n\n";
        break;

    // ── Phase 64: SAVEPOINT ──────────────────────────────────────
    case milansql::CommandType::SAVEPOINT:
        try {
            engine.createSavepoint(cmd.savepointName);
            std::cout << "  SAVEPOINT '" << cmd.savepointName << "' gesetzt.\n\n";
        } catch (const std::exception& e) {
            std::cout << "  FEHLER: " << e.what() << "\n\n";
        }
        break;

    case milansql::CommandType::ROLLBACK_TO_SAVEPOINT:
        try {
            engine.rollbackToSavepoint(cmd.savepointName);
            std::cout << "  ROLLBACK TO SAVEPOINT '" << cmd.savepointName << "' durchgeführt.\n\n";
        } catch (const std::exception& e) {
            std::cout << "  FEHLER: " << e.what() << "\n\n";
        }
        break;

    case milansql::CommandType::RELEASE_SAVEPOINT:
        try {
            engine.releaseSavepoint(cmd.savepointName);
            std::cout << "  RELEASE SAVEPOINT '" << cmd.savepointName << "'.\n\n";
        } catch (const std::exception& e) {
            std::cout << "  FEHLER: " << e.what() << "\n\n";
        }
        break;

    // ── Phase 65: LOCK TABLE / UNLOCK TABLES / SHOW LOCKS ───────
    case milansql::CommandType::LOCK_TABLE:
        try {
            if (cmd.tableName.empty() || cmd.lockType.empty()) {
                std::cout << "  Fehler: LOCK TABLE name READ|WRITE\n\n";
                break;
            }
            engine.lockTable(cmd.tableName, cmd.lockType);
            std::cout << "  LOCK TABLE '" << cmd.tableName
                      << "' (" << cmd.lockType << ") gesetzt.\n\n";
        } catch (const std::exception& e) {
            std::cout << "  FEHLER: " << e.what() << "\n\n";
        }
        break;

    case milansql::CommandType::UNLOCK_TABLES:
        engine.unlockTables();
        std::cout << "  UNLOCK TABLES — alle Tabellensperren freigegeben.\n\n";
        break;

    case milansql::CommandType::SHOW_LOCKS: {
        auto locks = engine.showLockInfo();
        if (locks.empty()) {
            std::cout << "  Keine aktiven Sperren.\n\n";
        } else {
            std::cout << "\n";
            milansql::Table lt("", {milansql::Column("Type","TEXT"),
                                    milansql::Column("Target","TEXT"),
                                    milansql::Column("Mode","TEXT"),
                                    milansql::Column("Thread","TEXT")});
            for (const auto& line : locks) {
                // Format: "ROW  | table:key | WRITE | thread:..."
                // or     "TBL  | table     | READ  | thread:..."
                auto split = [](const std::string& s, char d) {
                    std::vector<std::string> parts;
                    std::string cur;
                    for (char c : s) {
                        if (c == d) { parts.push_back(cur); cur.clear(); }
                        else cur += c;
                    }
                    parts.push_back(cur);
                    return parts;
                };
                auto parts = split(line, '|');
                auto trim = [](std::string s) {
                    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
                    while (!s.empty() && s.back() == ' ') s.pop_back();
                    return s;
                };
                std::string type   = parts.size() > 0 ? trim(parts[0]) : "";
                std::string target = parts.size() > 1 ? trim(parts[1]) : "";
                std::string mode   = parts.size() > 2 ? trim(parts[2]) : "";
                std::string thread = parts.size() > 3 ? trim(parts[3]) : "";
                lt.insert(milansql::Row({type, target, mode, thread}));
            }
            dispatch_printTable(lt, -1, 0);
        }
        break;
    }

    case milansql::CommandType::CREATE_TRIGGER: {
        if (cmd.triggerName.empty() || cmd.triggerTiming.empty() ||
            cmd.triggerEvent.empty() || cmd.triggerTable.empty()) {
            std::cout << "  Fehler: CREATE TRIGGER name BEFORE/AFTER INSERT/UPDATE/DELETE ON tbl FOR EACH ROW BEGIN ... END\n";
            break;
        }
        milansql::TriggerDef def;
        def.name      = cmd.triggerName;
        def.timing    = cmd.triggerTiming;
        def.event     = cmd.triggerEvent;
        def.tableName = cmd.triggerTable;
        def.body      = cmd.triggerBody;
        engine.createTrigger(def);
        saveTriggFn();
        std::cout << "  Trigger '" << def.name << "' erstellt.\n\n";
        break;
    }

    case milansql::CommandType::DROP_TRIGGER: {
        if (cmd.triggerName.empty()) {
            std::cout << "  Fehler: DROP TRIGGER triggername\n"; break;
        }
        engine.dropTrigger(cmd.triggerName);
        saveTriggFn();
        std::cout << "  Trigger '" << cmd.triggerName << "' geloescht.\n\n";
        break;
    }

    case milansql::CommandType::SHOW_TRIGGERS: {
        auto triggers = engine.showTriggers(cmd.showTriggersTable);
        if (triggers.empty()) {
            std::cout << "  Keine Trigger";
            if (!cmd.showTriggersTable.empty())
                std::cout << " auf '" << cmd.showTriggersTable << "'";
            std::cout << ".\n\n";
        } else {
            std::cout << "\n";
            for (const auto& t : triggers) {
                std::cout << "  " << t.name << " | " << t.timing
                          << " " << t.event << " ON " << t.tableName << "\n";
            }
            std::cout << "\n  " << triggers.size() << " Trigger\n\n";
        }
        break;
    }

    case milansql::CommandType::CREATE_PROCEDURE: {
        if (cmd.procedureName.empty()) {
            std::cout << "  Fehler: CREATE PROCEDURE name(params) BEGIN...END\n";
            break;
        }
        milansql::ProcedureDef def;
        def.name   = cmd.procedureName;
        def.params = cmd.procedureParams;
        def.body   = cmd.procedureBody;
        engine.createProcedure(def);
        saveProceduresFn();
        std::cout << "  Procedure '" << def.name << "' erstellt.\n\n";
        break;
    }

    case milansql::CommandType::DROP_PROCEDURE: {
        if (cmd.procedureName.empty()) {
            std::cout << "  Fehler: DROP PROCEDURE name\n"; break;
        }
        engine.dropProcedure(cmd.procedureName);
        saveProceduresFn();
        std::cout << "  Procedure '" << cmd.procedureName << "' geloescht.\n\n";
        break;
    }

    case milansql::CommandType::SHOW_PROCEDURES: {
        auto procs = engine.showProcedures();
        if (procs.empty()) {
            std::cout << "  Keine Procedures.\n\n";
        } else {
            std::cout << "\n";
            for (const auto& p : procs) {
                std::string paramStr;
                for (size_t pi = 0; pi < p.params.size(); ++pi) {
                    if (pi > 0) paramStr += ", ";
                    paramStr += p.params[pi].first + " " + p.params[pi].second;
                }
                std::cout << "  " << p.name << "(" << paramStr << ")\n";
            }
            std::cout << "\n  " << procs.size() << " Procedure(n)\n\n";
        }
        break;
    }

    case milansql::CommandType::CALL_PROCEDURE: {
        if (cmd.procedureName.empty()) {
            std::cout << "  Fehler: CALL name(args)\n"; break;
        }
        try {
            std::string body = engine.getProcedureBody(cmd.procedureName, cmd.callArgs);
            ProcState state;
            ProcExec exec{state, engine, persistFn};
            exec.execBody(body);
        } catch (const std::exception& ex) {
            std::cout << "  FEHLER (CALL): " << ex.what() << "\n\n";
        }
        break;
    }

    case milansql::CommandType::PREPARE_STMT: {
        if (cmd.preparedName.empty() || cmd.preparedSql.empty()) {
            std::cout << "  Fehler: PREPARE name AS sql\n\n"; break;
        }
        engine.prepareStmt(cmd.preparedName, cmd.preparedSql);
        std::cout << "  Statement '" << cmd.preparedName << "' vorbereitet ("
                  << std::count(cmd.preparedSql.begin(), cmd.preparedSql.end(), '?')
                  << " Parameter).\n\n";
        break;
    }

    case milansql::CommandType::EXECUTE_STMT: {
        if (cmd.preparedName.empty()) {
            std::cout << "  Fehler: EXECUTE name(args)\n\n"; break;
        }
        try {
            std::string boundSql = engine.bindPrepared(
                cmd.preparedName, cmd.execArgs);
            milansql::Parser innerParser;
            milansql::ParsedCommand sc = innerParser.parse(boundSql);
            for (const auto& sq : sc.subqueries) {
                if (sq.condIdx < sc.whereConds.size()) {
                    sc.whereConds[sq.condIdx].inList =
                        engine.subqueryValues(sq.subTable, sq.subCol,
                                              sq.subWhere, sq.subWhereLogic);
                }
            }
            if (sc.type == milansql::CommandType::SELECT) {
                milansql::Table result;
                if (!sc.whereConds.empty()) {
                    auto qr = engine.selectWhere(sc.tableName,
                        sc.whereConds, sc.whereLogic);
                    result = std::move(qr.table);
                } else {
                    result = engine.selectAll(sc.tableName).clone();
                }
                if (!sc.selectColumns.empty())
                    result = result.project(sc.selectColumns);
                if (!sc.orderByCols.empty())
                    result.sortByMulti(sc.orderByCols);
                dispatch_printTable(result, sc.limit, sc.limitOffset);
            } else if (sc.type == milansql::CommandType::UPDATE) {
                std::size_t n = 0;
                if (sc.whereColumn.empty()) {
                    n = engine.updateAll(sc.tableName,
                        sc.updateCols, sc.updateVals);
                } else {
                    n = engine.updateWhere(sc.tableName,
                        sc.updateCols, sc.updateVals,
                        sc.whereColumn, sc.whereValue);
                }
                std::cout << "  " << n << " Zeile(n) aktualisiert.\n\n";
                persistFn();
            } else if (sc.type == milansql::CommandType::INSERT) {
                const auto& rows45 = sc.multiValues.empty()
                    ? std::vector<std::vector<std::string>>{sc.values}
                    : sc.multiValues;
                for (const auto& vals : rows45)
                    engine.insertRow(sc.tableName, vals);
                persistFn();
                std::cout << "  " << rows45.size()
                          << " Zeile(n) eingefuegt.\n\n";
            } else if (sc.type == milansql::CommandType::DELETE) {
                std::size_t n = 0;
                if (sc.whereColumn.empty()) {
                    n = engine.deleteAll(sc.tableName);
                } else {
                    n = engine.deleteWhere(sc.tableName,
                        sc.whereColumn, sc.whereValue);
                }
                std::cout << "  " << n << " Zeile(n) geloescht.\n\n";
                persistFn();
            } else {
                std::cout << "  [EXECUTE] Unbekannter Befehl in Statement: '"
                          << boundSql << "'\n\n";
            }
        } catch (const std::runtime_error& ex45) {
            std::cout << "  FEHLER: " << ex45.what() << "\n\n";
        }
        break;
    }

    case milansql::CommandType::DEALLOCATE_STMT: {
        if (cmd.preparedName.empty()) {
            std::cout << "  Fehler: DEALLOCATE PREPARE name\n\n"; break;
        }
        engine.deallocateStmt(cmd.preparedName);
        std::cout << "  Statement '" << cmd.preparedName << "' freigegeben.\n\n";
        break;
    }

    case milansql::CommandType::SHOW_PREPARED: {
        auto stmts = engine.showPrepared();
        if (stmts.empty()) {
            std::cout << "  Keine prepared statements.\n\n";
        } else {
            for (const auto& s : stmts) {
                std::cout << "  " << s.name << " | "
                          << s.paramCount << " Parameter | "
                          << s.sql << "\n";
            }
            std::cout << "\n";
        }
        break;
    }

    case milansql::CommandType::CREATE_USER: {
        try {
            engine.createUser(cmd.userName, cmd.userPassword);
            std::cout << "User '" << cmd.userName << "' erstellt.\n";
        } catch (const std::runtime_error& e) {
            std::cout << "FEHLER: " << e.what() << "\n";
        }
        break;
    }

    case milansql::CommandType::DROP_USER: {
        try {
            engine.dropUser(cmd.userName);
            std::cout << "User '" << cmd.userName << "' geloescht.\n";
        } catch (const std::runtime_error& e) {
            std::cout << "FEHLER: " << e.what() << "\n";
        }
        break;
    }

    case milansql::CommandType::SHOW_USERS: {
        auto users = engine.showUsers();
        for (const auto& u : users) {
            std::string role = (u.name == "root") ? " [superuser]" : "";
            std::cout << u.name << role << "\n";
        }
        break;
    }

    case milansql::CommandType::GRANT_PRIV: {
        try {
            engine.grantPrivilege(cmd.grantTargetUser, cmd.grantTable, cmd.grantPrivs);
            std::string privStr;
            for (size_t i = 0; i < cmd.grantPrivs.size(); ++i) {
                if (i > 0) privStr += ", ";
                privStr += cmd.grantPrivs[i];
            }
            std::cout << privStr << " ON " << cmd.grantTable
                      << " granted to " << cmd.grantTargetUser << ".\n";
        } catch (const std::runtime_error& e) {
            std::cout << "FEHLER: " << e.what() << "\n";
        }
        break;
    }

    case milansql::CommandType::REVOKE_PRIV: {
        try {
            engine.revokePrivilege(cmd.grantTargetUser, cmd.grantTable, cmd.grantPrivs);
            std::cout << "Revoked from " << cmd.grantTargetUser << ".\n";
        } catch (const std::runtime_error& e) {
            std::cout << "FEHLER: " << e.what() << "\n";
        }
        break;
    }

    case milansql::CommandType::SHOW_GRANTS: {
        try {
            auto grants = engine.showGrants(cmd.grantTargetUser);
            if (grants.empty())
                std::cout << "Keine Rechte fuer " << cmd.grantTargetUser << ".\n";
            else
                for (const auto& g : grants) std::cout << g << "\n";
        } catch (const std::runtime_error& e) {
            std::cout << "FEHLER: " << e.what() << "\n";
        }
        break;
    }

    case milansql::CommandType::CONNECT_USER: {
        if (engine.connectUser(cmd.userName, cmd.userPassword)) {
            std::cout << "Verbunden als " << cmd.userName << ".\n";
        } else {
            std::cout << "FEHLER: Falscher Benutzername oder Passwort.\n";
        }
        break;
    }

    case milansql::CommandType::DISCONNECT_USER: {
        engine.disconnectUser();
        std::cout << "Verbindung getrennt. Aktiver User: root.\n";
        break;
    }

    // ── Phase 49: Full-Text Search ─────────────────────────────
    case milansql::CommandType::CREATE_FULLTEXT_INDEX: {
        if (cmd.fulltextIndexName.empty() || cmd.tableName.empty() || cmd.fulltextCols.empty()) {
            std::cout << "  Fehler: CREATE FULLTEXT INDEX name ON tabelle (spalte, ...)\n"; break;
        }
        engine.createFulltextIndex(cmd.fulltextIndexName, cmd.tableName, cmd.fulltextCols);
        std::cout << "  Fulltext-Index '" << cmd.fulltextIndexName << "' erstellt.\n\n";
        break;
    }

    case milansql::CommandType::DROP_FULLTEXT_INDEX: {
        if (cmd.fulltextIndexName.empty()) {
            std::cout << "  Fehler: DROP FULLTEXT INDEX name ON tabelle\n"; break;
        }
        engine.dropFulltextIndex(cmd.fulltextIndexName);
        std::cout << "  Fulltext-Index '" << cmd.fulltextIndexName << "' geloescht.\n\n";
        break;
    }

    // ── Phase 51: Schema Management ─────────────────────────────

    case milansql::CommandType::CREATE_SCHEMA: {
        if (cmd.schemaName.empty()) {
            std::cout << "  Fehler: CREATE SCHEMA name\n"; break;
        }
        engine.createSchema(cmd.schemaName);
        // Save schemas to file
        {
            std::ofstream sf("database.schemas");
            if (sf) for (const auto& s : engine.showSchemas()) sf << s << "\n";
        }
        std::cout << "  Schema '" << cmd.schemaName << "' erstellt.\n\n";
        break;
    }

    case milansql::CommandType::DROP_SCHEMA: {
        if (cmd.schemaName.empty()) {
            std::cout << "  Fehler: DROP SCHEMA name\n"; break;
        }
        try {
            engine.dropSchema(cmd.schemaName);
            persistFn();  // Save tables (some may have been deleted)
            {
                std::ofstream sf("database.schemas");
                if (sf) for (const auto& s : engine.showSchemas()) sf << s << "\n";
            }
            std::cout << "  Schema '" << cmd.schemaName << "' geloescht.\n\n";
        } catch (const std::exception& ex) {
            std::cout << "  FEHLER: " << ex.what() << "\n\n";
        }
        break;
    }

    case milansql::CommandType::SHOW_SCHEMAS: {
        auto schemas = engine.showSchemas();
        std::cout << "  Schemas:\n";
        for (const auto& s : schemas)
            std::cout << "    " << s
                      << (s == engine.getCurrentSchema() ? "  (aktiv)" : "") << "\n";
        std::cout << "\n";
        break;
    }

    case milansql::CommandType::USE_SCHEMA: {
        if (cmd.schemaName.empty()) {
            std::cout << "  Fehler: USE schemaname\n"; break;
        }
        engine.useSchema(cmd.schemaName);
        {
            std::ofstream sf("database.schemas");
            if (sf) for (const auto& s : engine.showSchemas()) sf << s << "\n";
        }
        std::cout << "  Schema '" << cmd.schemaName << "' aktiviert.\n\n";
        break;
    }

    case milansql::CommandType::SHOW_TABLES_IN: {
        if (cmd.schemaName.empty()) {
            std::cout << "  Fehler: SHOW TABLES IN schemaname\n"; break;
        }
        auto tables = engine.showTablesInSchema(cmd.schemaName);
        std::cout << "  Tabellen in Schema '" << cmd.schemaName << "':\n";
        if (tables.empty()) std::cout << "    (keine)\n";
        for (const auto& t : tables) std::cout << "    " << t << "\n";
        std::cout << "\n";
        break;
    }

    // ── Phase 57: BACKUP DATABASE ──────────────────────────────
    case milansql::CommandType::BACKUP_DATABASE: {
        std::string msg = milansql::MilanBackup::dumpDatabase(engine, cmd.backupFile);
        std::cout << "  " << msg << "\n\n";
        break;
    }

    // ── Phase 57: BACKUP TABLE ─────────────────────────────────
    case milansql::CommandType::BACKUP_TABLE: {
        if (cmd.tableName.empty()) {
            std::cout << "  Fehler: BACKUP TABLE name TO 'datei.sql'\n\n"; break;
        }
        std::string msg = milansql::MilanBackup::dumpTable(engine, cmd.tableName, cmd.backupFile);
        std::cout << "  " << msg << "\n\n";
        break;
    }

    // ── Phase 57: RESTORE DATABASE ─────────────────────────────
    case milansql::CommandType::RESTORE_DATABASE: {
        if (cmd.backupFile.empty()) {
            std::cout << "  Fehler: RESTORE DATABASE FROM 'datei.sql'\n\n"; break;
        }
        std::cout << "  Starte Restore aus '" << cmd.backupFile << "'...\n";

        // Output waehrend Restore unterdruecken (nur Zusammenfassung zeigen)
        std::streambuf* oldBuf = std::cout.rdbuf();
        std::ostringstream devNull;
        std::cout.rdbuf(devNull.rdbuf());

        auto noopPersist = []() {};
        auto executeSQL  = [&](const std::string& sql) {
            try {
                milansql::ParsedCommand rcmd = parser.parse(sql);
                dispatchCommand(rcmd, engine, parser, sql,
                                noopPersist, saveProceduresFn, saveTriggFn,
                                getProcessListFn);
            } catch (...) {}
        };

        std::string msg = milansql::MilanBackup::restoreDatabase(cmd.backupFile, executeSQL);

        std::cout.rdbuf(oldBuf);

        persistFn();
        std::cout << "  Restore abgeschlossen: " << msg << "\n\n";
        break;
    }

    // ── Phase 57: SHOW BACKUPS ─────────────────────────────────
    case milansql::CommandType::SHOW_BACKUPS: {
        auto files = milansql::MilanBackup::listBackups();
        if (files.empty()) {
            std::cout << "  (Keine .sql Backup-Dateien im aktuellen Verzeichnis)\n\n";
        } else {
            std::cout << "  Backup-Dateien:\n";
            for (const auto& f : files)
                std::cout << "    " << f << "\n";
            std::cout << "\n";
        }
        break;
    }

    // ── Phase 58: BENCHMARK ────────────────────────────────────
    case milansql::CommandType::BENCHMARK: {
        if (cmd.benchmarkSql.empty() || cmd.benchmarkIter <= 0) {
            std::cout << "  Fehler: BENCHMARK <n> <SQL>\n\n"; break;
        }

        using clock_t = std::chrono::high_resolution_clock;
        double minMs   = std::numeric_limits<double>::max();
        double maxMs   = 0.0;
        double totalMs = 0.0;

        // Output während Benchmark unterdrücken
        std::streambuf* oldBuf = std::cout.rdbuf();
        std::ostringstream devNull;
        auto noop = []() {};

        for (int bi = 0; bi < cmd.benchmarkIter; ++bi) {
            std::cout.rdbuf(devNull.rdbuf());
            auto t0 = clock_t::now();

            try {
                milansql::ParsedCommand bcmd = parser.parse(cmd.benchmarkSql);
                dispatchCommand(bcmd, engine, parser, cmd.benchmarkSql,
                                noop, noop, noop, nullptr);
            } catch (...) {}

            auto t1 = clock_t::now();
            std::cout.rdbuf(oldBuf);

            double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            totalMs += ms;
            if (ms < minMs) minMs = ms;
            if (ms > maxMs) maxMs = ms;
        }

        std::cout.rdbuf(oldBuf);  // Sicherheits-Restore

        double avgMs = totalMs / cmd.benchmarkIter;
        double qps   = (totalMs > 0.0) ? (cmd.benchmarkIter * 1000.0 / totalMs) : 0.0;

        auto fmtMs = [](double ms) -> std::string {
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(3) << ms << "ms";
            return ss.str();
        };
        std::ostringstream qpsStr;
        qpsStr << std::fixed << std::setprecision(0) << qps;

        std::cout << "\n";
        std::cout << "  Benchmark: " << cmd.benchmarkIter << " Iterationen\n";
        std::cout << "  SQL: " << cmd.benchmarkSql << "\n";
        std::cout << "  \u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\n";
        std::cout << "  Gesamtzeit:    " << fmtMs(totalMs) << "\n";
        std::cout << "  Durchschnitt:  " << fmtMs(avgMs) << " pro Query\n";
        std::cout << "  Min:           " << fmtMs(minMs) << "\n";
        std::cout << "  Max:           " << fmtMs(maxMs) << "\n";
        std::cout << "  Queries/sec:   " << qpsStr.str() << "\n\n";

        // Pool-Statistiken aktualisieren
        milansql::g_poolStats.totalRequests.fetch_add(cmd.benchmarkIter);
        long long totalUs = static_cast<long long>(totalMs * 1000.0);
        milansql::g_poolStats.totalQueryTimeUs.fetch_add(totalUs);

        break;
    }

    // ── Phase 59: Replication Commands ───────────────────────────

    case milansql::CommandType::SHOW_MASTER_STATUS: {
        if (!milansql::g_replState.isMaster.load()) {
            std::cout << "  FEHLER: Dieser Server laeuft nicht im Master-Modus.\n\n";
            break;
        }
        long long pos = milansql::g_binlogGetPosFn
                        ? milansql::g_binlogGetPosFn() : 0;
        int slaves    = milansql::g_replState.connectedSlaves.load();
        struct KV { std::string k, v; };
        std::vector<KV> rows = {
            {"Binlog-Datei",    milansql::g_replState.binlogFile},
            {"Position",        std::to_string(pos)},
            {"Slaves aktiv",    std::to_string(slaves)},
            {"Repl-Port",       "(siehe --repl-port)"},
        };
        size_t w1 = 14, w2 = 24;
        for (auto& r : rows) {
            if (r.k.size() > w1) w1 = r.k.size();
            if (r.v.size() > w2) w2 = r.v.size();
        }
        std::string hline(w1 + w2 + 7, '\xe2');
        auto bar = [&](char lc, char mc, char rc) {
            std::string s;
            s += lc; s += std::string(w1 + 2, '-');
            s += mc; s += std::string(w2 + 2, '-');
            s += rc; return s;
        };
        std::cout << "\n  " << bar('+','+','+') << "\n";
        std::cout << "  | " << std::left << std::setw(static_cast<int>(w1)) << "Eigenschaft"
                  << " | " << std::setw(static_cast<int>(w2)) << "Wert" << " |\n";
        std::cout << "  " << bar('+','+','+') << "\n";
        for (auto& r : rows)
            std::cout << "  | " << std::left << std::setw(static_cast<int>(w1)) << r.k
                      << " | " << std::setw(static_cast<int>(w2)) << r.v << " |\n";
        std::cout << "  " << bar('+','+','+') << "\n\n";
        break;
    }

    case milansql::CommandType::SHOW_SLAVE_STATUS: {
        if (!milansql::g_replState.isSlave.load()) {
            std::cout << "  FEHLER: Dieser Server laeuft nicht im Slave-Modus.\n\n";
            break;
        }
        std::string status;
        { std::lock_guard<std::mutex> lk(milansql::g_replState.statusMu);
          status = milansql::g_replState.slaveStatus; }
        std::string lagStr = std::to_string(milansql::g_replState.slaveLagMs.load()) + "ms";
        struct KV { std::string k, v; };
        std::vector<KV> rows = {
            {"Master-Host",   milansql::g_replState.masterHost},
            {"Master-Port",   std::to_string(milansql::g_replState.masterPort)},
            {"Slave-Status",  status},
            {"Slave-Position",std::to_string(milansql::g_replState.slavePos.load())},
            {"Lag",           lagStr},
            {"Read-Only",     "Ja"},
        };
        size_t w1 = 14, w2 = 24;
        for (auto& r : rows) {
            if (r.k.size() > w1) w1 = r.k.size();
            if (r.v.size() > w2) w2 = r.v.size();
        }
        auto bar = [&](char lc, char mc, char rc) {
            std::string s;
            s += lc; s += std::string(w1 + 2, '-');
            s += mc; s += std::string(w2 + 2, '-');
            s += rc; return s;
        };
        std::cout << "\n  " << bar('+','+','+') << "\n";
        std::cout << "  | " << std::left << std::setw(static_cast<int>(w1)) << "Eigenschaft"
                  << " | " << std::setw(static_cast<int>(w2)) << "Wert" << " |\n";
        std::cout << "  " << bar('+','+','+') << "\n";
        for (auto& r : rows)
            std::cout << "  | " << std::left << std::setw(static_cast<int>(w1)) << r.k
                      << " | " << std::setw(static_cast<int>(w2)) << r.v << " |\n";
        std::cout << "  " << bar('+','+','+') << "\n\n";
        break;
    }

    case milansql::CommandType::SHOW_BINLOG: {
        if (!milansql::g_binlogReadLastFn) {
            std::cout << "  FEHLER: Kein Binlog verfuegbar (Master-Modus erforderlich).\n\n";
            break;
        }
        auto entries = milansql::g_binlogReadLastFn(20);
        if (entries.empty()) {
            std::cout << "  Binlog ist leer.\n\n";
            break;
        }
        // column widths
        size_t wPos = 3, wTs = 19, wSql = 40;
        for (auto& e : entries) {
            size_t ps = std::to_string(e.pos).size();
            if (ps > wPos) wPos = ps;
            if (e.sql.size() > wSql) wSql = std::min(e.sql.size(), size_t(60));
        }
        auto bar = [&]() {
            return std::string("+") + std::string(wPos + 2, '-') + "+"
                 + std::string(wTs  + 2, '-') + "+"
                 + std::string(wSql + 2, '-') + "+";
        };
        auto trunc = [](const std::string& s, size_t n) {
            if (s.size() <= n) return s;
            return s.substr(0, n - 3) + "...";
        };
        std::cout << "\n  " << bar() << "\n";
        std::cout << "  | " << std::left << std::setw(static_cast<int>(wPos)) << "Pos"
                  << " | " << std::setw(static_cast<int>(wTs))  << "Timestamp"
                  << " | " << std::setw(static_cast<int>(wSql)) << "SQL" << " |\n";
        std::cout << "  " << bar() << "\n";
        for (auto& e : entries) {
            std::cout << "  | " << std::left
                      << std::setw(static_cast<int>(wPos)) << e.pos
                      << " | " << std::setw(static_cast<int>(wTs))  << e.timestamp
                      << " | " << std::setw(static_cast<int>(wSql)) << trunc(e.sql, wSql)
                      << " |\n";
        }
        std::cout << "  " << bar() << "\n";
        std::cout << "  " << entries.size() << " Eintrag/Eintraege.\n\n";
        break;
    }

    case milansql::CommandType::STOP_SLAVE: {
        if (!milansql::g_replState.isSlave.load()) {
            std::cout << "  FEHLER: Kein Slave-Modus aktiv.\n\n"; break;
        }
        if (milansql::g_stopSlaveHook) milansql::g_stopSlaveHook();
        std::cout << "  Replikation gestoppt.\n\n";
        break;
    }

    case milansql::CommandType::START_SLAVE: {
        if (!milansql::g_replState.isSlave.load()) {
            std::cout << "  FEHLER: Kein Slave-Modus aktiv.\n\n"; break;
        }
        if (milansql::g_startSlaveHook) milansql::g_startSlaveHook();
        std::cout << "  Replikation gestartet.\n\n";
        break;
    }

    // ── Phase 60: LOAD DATA INFILE ────────────────────────────────

    case milansql::CommandType::LOAD_DATA: {
        if (dispatch_slaveReadOnly()) {
            std::cout << "  FEHLER: Read-only: Slave akzeptiert keine Schreiboperationen\n\n";
            break;
        }
        if (cmd.csvFile.empty() || cmd.tableName.empty()) {
            std::cout << "  Fehler: LOAD DATA INFILE 'file.csv' INTO TABLE name"
                         " [SEPARATOR ','] [SKIP HEADER]\n\n";
            break;
        }
        try {
            char sep = milansql::CsvUtils::parseSepChar(
                cmd.csvSeparator.empty() ? "," : cmd.csvSeparator);
            auto rows = milansql::CsvUtils::readFile(
                cmd.csvFile, sep, cmd.csvSkipHeader);

            size_t inserted = 0, skipped = 0;
            for (const auto& row : rows) {
                try {
                    engine.insertRow(cmd.tableName, row);
                    ++inserted;
                } catch (const std::exception& rowEx) {
                    ++skipped;
                    (void)rowEx; // silently skip constraint violations
                }
            }
            persistFn();
            dispatch_binlogWrite(eingabe);
            engine.invalidateCache(cmd.tableName);
            std::cout << "  " << inserted << " Zeile(n) importiert aus '"
                      << cmd.csvFile << "' in '" << cmd.tableName << "'";
            if (skipped > 0)
                std::cout << " (" << skipped << " Zeile(n) uebersprungen)";
            std::cout << ".\n\n";
        } catch (const std::exception& ex) {
            std::cout << "  FEHLER (LOAD DATA): " << ex.what() << "\n\n";
        }
        break;
    }

    case milansql::CommandType::INTO_OUTFILE: {
        // Should not normally reach here — handled inside SELECT case.
        // Fallback if parser set type = INTO_OUTFILE (unused path).
        std::cout << "  Fehler: SELECT ... INTO OUTFILE nur als SELECT-Suffix.\n\n";
        break;
    }

    case milansql::CommandType::SHOW_DATAFILES: {
        auto files = milansql::CsvUtils::listCsvFiles();
        if (files.empty()) {
            std::cout << "  (Keine .csv/.tsv Dateien im aktuellen Verzeichnis)\n\n";
        } else {
            // Tabelle: Name | Groesse
            struct FileRow { std::string name; std::string size; };
            std::vector<FileRow> rows;
            size_t w1 = 4, w2 = 6; // "Name", "Groesse"
            for (const auto& f : files) {
                std::string sz = "?";
                try {
                    auto bytes = std::filesystem::file_size(f);
                    if (bytes < 1024)
                        sz = std::to_string(bytes) + " B";
                    else if (bytes < 1024 * 1024)
                        sz = std::to_string(bytes / 1024) + " KB";
                    else
                        sz = std::to_string(bytes / (1024 * 1024)) + " MB";
                } catch (...) {}
                if (f.size()  > w1) w1 = f.size();
                if (sz.size() > w2) w2 = sz.size();
                rows.push_back({f, sz});
            }
            auto bar = [&]() {
                return std::string("+") + std::string(w1 + 2, '-') + "+"
                     + std::string(w2 + 2, '-') + "+";
            };
            std::cout << "\n  " << bar() << "\n";
            std::cout << "  | " << std::left
                      << std::setw(static_cast<int>(w1)) << "Name"
                      << " | " << std::setw(static_cast<int>(w2)) << "Groesse"
                      << " |\n";
            std::cout << "  " << bar() << "\n";
            for (auto& r : rows)
                std::cout << "  | "
                          << std::left << std::setw(static_cast<int>(w1)) << r.name
                          << " | " << std::setw(static_cast<int>(w2)) << r.size
                          << " |\n";
            std::cout << "  " << bar() << "\n";
            std::cout << "  " << rows.size() << " Datei(en).\n\n";
        }
        break;
    }

    // ── Phase 61: CREATE EVENT ────────────────────────────────────
    case milansql::CommandType::CREATE_EVENT: {
        if (cmd.eventName.empty() || cmd.eventSql.empty()) {
            std::cout << "  Fehler: CREATE EVENT name ON SCHEDULE "
                         "EVERY n UNIT [AT 'HH:MM:SS'] DO sql\n\n";
            break;
        }
        milansql::EventDef ev;
        ev.name      = cmd.eventName;
        ev.sql       = cmd.eventSql;
        ev.enabled   = true;
        ev.recurring = cmd.eventRecurring;

        if (ev.recurring) {
            // Convert interval to seconds
            long long n = (cmd.eventIntervalN > 0) ? cmd.eventIntervalN : 1;
            std::string unit = cmd.eventIntervalUnit;
            long long factor = 1;
            if      (unit == "MINUTE" || unit == "MINUTES") factor = 60;
            else if (unit == "HOUR"   || unit == "HOURS")   factor = 3600;
            else if (unit == "DAY"    || unit == "DAYS")     factor = 86400;
            else if (unit == "WEEK"   || unit == "WEEKS")    factor = 604800;
            else if (unit == "MONTH"  || unit == "MONTHS")   factor = 2592000;
            ev.intervalSecs = n * factor;
            ev.hasAt  = cmd.eventHasAt;
            ev.atTime = cmd.eventAtTime;
            ev.nextRun = milansql::EventScheduler::computeNextRunRecurring(
                ev.intervalSecs, ev.hasAt, ev.atTime);
        } else {
            ev.atTime  = cmd.eventAtTime;
            ev.nextRun = milansql::EventScheduler::computeNextRunOnce(ev.atTime);
            // If time is already in the past, disable immediately
            if (ev.nextRun <= std::time(nullptr)) {
                ev.enabled = false;
            }
        }

        if (milansql::g_eventScheduler) {
            milansql::g_eventScheduler->createEvent(ev);
        } else {
            // REPL without scheduler started — store in a simple fallback list
            // (in practice, main.cpp always creates the scheduler)
        }
        std::cout << "  Event '" << ev.name << "' erstellt";
        if (!ev.enabled) std::cout << " (Zeitpunkt in der Vergangenheit — deaktiviert)";
        std::cout << ".\n\n";
        break;
    }

    // ── Phase 61: DROP EVENT ──────────────────────────────────────
    case milansql::CommandType::DROP_EVENT: {
        if (cmd.eventName.empty()) {
            std::cout << "  Fehler: DROP EVENT name\n\n"; break;
        }
        bool dropped = milansql::g_eventScheduler &&
                       milansql::g_eventScheduler->dropEvent(cmd.eventName);
        if (dropped)
            std::cout << "  Event '" << cmd.eventName << "' geloescht.\n\n";
        else
            std::cout << "  FEHLER: Event '" << cmd.eventName << "' nicht gefunden.\n\n";
        break;
    }

    // ── Phase 61: SHOW EVENTS ─────────────────────────────────────
    case milansql::CommandType::SHOW_EVENTS: {
        auto evts = milansql::g_eventScheduler
            ? milansql::g_eventScheduler->getEvents()
            : std::vector<milansql::EventDef>{};

        if (evts.empty()) {
            std::cout << "  (Keine Events definiert)\n\n"; break;
        }

        // Column headers
        const std::string h0 = "Name", h1 = "Schedule", h2 = "Status";
        size_t w0 = h0.size(), w1 = h1.size(), w2 = h2.size();
        std::vector<std::array<std::string, 3>> rows;
        for (const auto& ev : evts) {
            std::string sched = milansql::EventScheduler::scheduleStr(ev);
            std::string status = ev.enabled ? "ENABLED" : "DISABLED";
            w0 = std::max(w0, ev.name.size());
            w1 = std::max(w1, sched.size());
            w2 = std::max(w2, status.size());
            rows.push_back({ev.name, sched, status});
        }

        auto bar = [&]() {
            return "+" + std::string(w0 + 2, '-') + "+"
                 + std::string(w1 + 2, '-') + "+"
                 + std::string(w2 + 2, '-') + "+";
        };
        auto printRow = [&](const std::string& c0, const std::string& c1,
                            const std::string& c2) {
            std::cout << "  | " << std::left
                      << std::setw(static_cast<int>(w0)) << c0 << " | "
                      << std::setw(static_cast<int>(w1)) << c1 << " | "
                      << std::setw(static_cast<int>(w2)) << c2 << " |\n";
        };
        std::cout << "\n  " << bar() << "\n";
        printRow(h0, h1, h2);
        std::cout << "  " << bar() << "\n";
        for (const auto& r : rows) printRow(r[0], r[1], r[2]);
        std::cout << "  " << bar() << "\n";
        std::cout << "  " << evts.size() << " Event(s).\n\n";

        // Show scheduler status
        bool on = milansql::g_eventScheduler &&
                  milansql::g_eventScheduler->isOn();
        std::cout << "  Event Scheduler: " << (on ? "ON" : "OFF") << "\n\n";
        break;
    }

    // ── Phase 61: ALTER EVENT name ENABLE/DISABLE ─────────────────
    case milansql::CommandType::ALTER_EVENT: {
        if (cmd.eventName.empty()) {
            std::cout << "  Fehler: ALTER EVENT name ENABLE|DISABLE\n\n"; break;
        }
        bool ok = milansql::g_eventScheduler &&
                  milansql::g_eventScheduler->setEventEnabled(
                      cmd.eventName, cmd.eventEnabled);
        if (ok)
            std::cout << "  Event '" << cmd.eventName << "' "
                      << (cmd.eventEnabled ? "aktiviert" : "deaktiviert") << ".\n\n";
        else
            std::cout << "  FEHLER: Event '" << cmd.eventName << "' nicht gefunden.\n\n";
        break;
    }

    // ── Phase 61: SET EVENT_SCHEDULER = ON/OFF ────────────────────
    case milansql::CommandType::SET_EVENT_SCHEDULER: {
        if (milansql::g_eventScheduler) {
            milansql::g_eventScheduler->setOn(cmd.eventSchedulerOn);
            std::cout << "  Event Scheduler "
                      << (cmd.eventSchedulerOn ? "aktiviert" : "deaktiviert") << ".\n\n";
        } else {
            std::cout << "  Event Scheduler nicht verfuegbar.\n\n";
        }
        break;
    }

    // ── Phase 69: Query Profiler ──────────────────────────────────
    case milansql::CommandType::PROFILE_ON:
        g_profiler.enable();
        std::cout << "  Query Profiler aktiviert.\n\n";
        break;

    case milansql::CommandType::PROFILE_OFF:
        g_profiler.disable();
        std::cout << "  Query Profiler deaktiviert.\n\n";
        break;

    case milansql::CommandType::SHOW_PROFILES:
        g_profiler.showProfiles();
        break;

    case milansql::CommandType::SHOW_PROFILE_FOR_QUERY:
        if (cmd.profileQueryId <= 0)
            std::cout << "  Fehler: SHOW PROFILE FOR QUERY n\n\n";
        else
            g_profiler.showProfile(cmd.profileQueryId);
        break;

    // ── Phase 70: Spatial Index ───────────────────────────────────
    case milansql::CommandType::CREATE_SPATIAL_INDEX: {
        if (cmd.indexName.empty() || cmd.tableName.empty() || cmd.indexColumns.empty()) {
            std::cout << "  Fehler: CREATE SPATIAL INDEX name ON table (col)\n\n";
            break;
        }
        try {
            engine.createIndex(cmd.tableName, cmd.indexColumns, cmd.indexName, "SPATIAL");
            persistFn();
            std::cout << "  Spatial-Index '" << cmd.indexName
                      << "' auf Tabelle '" << cmd.tableName
                      << "' (" << cmd.indexColumns[0] << ") erstellt.\n\n";
        } catch (const std::exception& e) {
            std::cout << "  FEHLER: " << e.what() << "\n\n";
        }
        break;
    }

    // ── Phase 71: MVCC ─────────────────────────────────────────────

    case milansql::CommandType::VACUUM:
    case milansql::CommandType::VACUUM_ANALYZE: {
        size_t cleaned = engine.vacuumAllTracked();
        std::cout << "  VACUUM: " << cleaned << " alte Version(en) bereinigt.\n";
        if (cmd.type == milansql::CommandType::VACUUM_ANALYZE)
            std::cout << "  ANALYZE: Tabellenstatistiken aktualisiert.\n";
        std::cout << "\n";
        persistFn();
        break;
    }

    // ── Phase 85: VACUUM <table> ──────────────────────────────
    case milansql::CommandType::VACUUM_TABLE: {
        if (cmd.tableName.empty()) {
            std::cout << "  Fehler: VACUUM <tabellenname>\n\n"; break;
        }
        try {
            size_t cleaned = engine.vacuumTableTracked(cmd.tableName);
            std::cout << "  VACUUM '" << cmd.tableName << "': "
                      << cleaned << " alte Version(en) bereinigt.\n\n";
            persistFn();
        } catch (const std::exception& e) {
            std::cout << "  FEHLER: " << e.what() << "\n\n";
        }
        break;
    }

    // ── Phase 85: VACUUM FULL ─────────────────────────────────
    case milansql::CommandType::VACUUM_FULL: {
        size_t cleaned = engine.vacuumAllTracked();
        std::cout << "  VACUUM FULL: " << cleaned << " alte Version(en) bereinigt (aggressive Komprimierung).\n\n";
        persistFn();
        break;
    }

    // ── Phase 85: CHECKPOINT ──────────────────────────────────
    case milansql::CommandType::CHECKPOINT: {
        engine.doCheckpoint();
        break;
    }

    // ── Phase 85: SHOW CHECKPOINT STATUS ─────────────────────
    case milansql::CommandType::SHOW_CHECKPOINT_STATUS: {
        engine.showCheckpointStatus();
        break;
    }

    // ── Phase 85: SET AUTO_CHECKPOINT = N ────────────────────
    case milansql::CommandType::SET_AUTO_CHECKPOINT: {
        std::string val = cmd.values.empty() ? "" : cmd.values[0];
        try {
            uint64_t n = std::stoull(val);
            engine.setAutoCheckpointInterval(n);
            std::cout << "  AUTO_CHECKPOINT Intervall gesetzt: " << n << " Transaktionen\n\n";
        } catch (...) {
            std::cout << "  Fehler: SET AUTO_CHECKPOINT = <zahl>\n\n";
        }
        break;
    }

    // ── Phase 85: SHOW VACUUM STATUS ─────────────────────────
    case milansql::CommandType::SHOW_VACUUM_STATUS: {
        engine.showVacuumStatus();
        break;
    }

    // ── Phase 85: SET AUTO_VACUUM = ON/OFF ───────────────────
    case milansql::CommandType::SET_AUTO_VACUUM: {
        std::string val = cmd.values.empty() ? "" : cmd.values[0];
        if (val == "ON" || val == "OFF") {
            engine.setAutoVacuumEnabled(val == "ON");
            std::cout << "  AUTO_VACUUM = " << val << "\n\n";
        } else {
            std::cout << "  Fehler: SET AUTO_VACUUM = ON | OFF\n\n";
        }
        break;
    }

    // ── Phase 85: SET AUTO_VACUUM_THRESHOLD = N ──────────────
    case milansql::CommandType::SET_AUTO_VACUUM_THRESHOLD: {
        std::string val = cmd.values.empty() ? "" : cmd.values[0];
        try {
            size_t n = std::stoull(val);
            engine.setAutoVacuumThreshold(n);
            std::cout << "  AUTO_VACUUM_THRESHOLD gesetzt: " << n << " Dead Tuples\n\n";
        } catch (...) {
            std::cout << "  Fehler: SET AUTO_VACUUM_THRESHOLD = <zahl>\n\n";
        }
        break;
    }

    case milansql::CommandType::SHOW_TRANSACTIONS:
        engine.showTransactions();
        break;

    case milansql::CommandType::SET_TRANSACTION_ISOLATION: {
        if (cmd.isolationLevel.empty()) {
            std::cout << "  Fehler: SET TRANSACTION ISOLATION LEVEL <level>\n\n";
            break;
        }
        engine.setIsolationLevel(cmd.isolationLevel);
        std::cout << "  Isolation Level gesetzt: " << cmd.isolationLevel << "\n\n";
        break;
    }

    // ── Phase 72: WAL Recovery Status ─────────────────────────────
    case milansql::CommandType::SHOW_RECOVERY_STATUS:
        engine.showRecoveryStatus();
        break;

    // ── Phase 72: Materialized Views ──────────────────────────────

    case milansql::CommandType::SHOW_MATERIALIZED_VIEWS:
        engine.showMaterializedViews();
        break;

    case milansql::CommandType::CREATE_MATERIALIZED_VIEW: {
        if (cmd.matViewName.empty() || cmd.matViewSql.empty()) {
            std::cout << "  Fehler: CREATE MATERIALIZED VIEW name AS SELECT ...\n\n";
            break;
        }
        try {
            milansql::ParsedCommand inner = parser.parse(cmd.matViewSql);
            milansql::Table result = dispatch_executeSelectToTable(engine, parser, inner);
            engine.createMaterializedView(cmd.matViewName, cmd.matViewSql,
                                          result.columns(), result.rows());
            // Persist matview definitions
            {
                std::ofstream mf("database.matviews", std::ios::app);
                if (mf) mf << cmd.matViewName << "\t" << cmd.matViewSql << "\n";
            }
            std::cout << "  Materialized View '" << cmd.matViewName
                      << "' erstellt (" << result.rowCount() << " Zeile(n)).\n\n";
        } catch (const std::exception& e) {
            std::cout << "  FEHLER: " << e.what() << "\n\n";
        }
        break;
    }

    case milansql::CommandType::REFRESH_MATERIALIZED_VIEW: {
        if (cmd.matViewName.empty()) {
            std::cout << "  Fehler: REFRESH MATERIALIZED VIEW name\n\n";
            break;
        }
        try {
            const auto& mv = engine.getMaterializedView(cmd.matViewName);
            milansql::ParsedCommand inner = parser.parse(mv.sql);
            milansql::Table result = dispatch_executeSelectToTable(engine, parser, inner);
            engine.setMaterializedViewData(cmd.matViewName, result.columns(), result.rows());
            std::cout << "  Materialized View '" << cmd.matViewName
                      << "' aktualisiert (" << result.rowCount() << " Zeile(n)).\n\n";
        } catch (const std::exception& e) {
            std::cout << "  FEHLER: " << e.what() << "\n\n";
        }
        break;
    }

    case milansql::CommandType::DROP_MATERIALIZED_VIEW: {
        if (cmd.matViewName.empty()) {
            std::cout << "  Fehler: DROP MATERIALIZED VIEW name\n\n";
            break;
        }
        try {
            engine.dropMaterializedView(cmd.matViewName);
            // Remove from database.matviews file by rewriting it
            {
                std::ifstream fin("database.matviews");
                std::string content;
                if (fin) {
                    std::string line;
                    while (std::getline(fin, line)) {
                        if (line.empty()) continue;
                        size_t tab = line.find('\t');
                        if (tab != std::string::npos && line.substr(0, tab) == cmd.matViewName)
                            continue;
                        content += line + "\n";
                    }
                    fin.close();
                    std::ofstream fout("database.matviews");
                    if (fout) fout << content;
                }
            }
            std::cout << "  Materialized View '" << cmd.matViewName << "' geloescht.\n\n";
        } catch (const std::exception& e) {
            std::cout << "  FEHLER: " << e.what() << "\n\n";
        }
        break;
    }

    // ── Phase 73: Buffer Pool Manager ────────────────────────────

    case milansql::CommandType::SHOW_BUFFER_POOL_STATUS:
        engine.showBufferPoolStatus();
        break;

    case milansql::CommandType::FLUSH_BUFFER_POOL: {
        // Flush all dirty pages to disk via storage save
        // We flush by calling persistFn (which saves the whole DB).
        // Also mark all pages clean via the engine.
        auto dirtyPages = engine.getDirtyBufferPages();
        persistFn();
        for (const auto& pg : dirtyPages)
            engine.markBufferPageClean(pg);
        std::cout << "  Buffer Pool geleert: " << dirtyPages.size()
                  << " Dirty Page(s) auf Disk geschrieben.\n\n";
        break;
    }

    case milansql::CommandType::SET_BUFFER_POOL_SIZE: {
        if (cmd.values.empty()) {
            std::cout << "  Fehler: SET BUFFER_POOL_SIZE = <MB>\n\n";
            break;
        }
        int mb = 128;
        try { mb = std::stoi(cmd.values[0]); } catch (...) {}
        if (mb < 1) mb = 1;
        engine.setBufferPoolSize(mb);
        std::cout << "  Buffer Pool Groesse gesetzt: " << mb << " MB\n\n";
        break;
    }

    // ── Phase 75: Row-Level Security ─────────────────────────
    case milansql::CommandType::CREATE_POLICY: {
        milansql::Engine::RlsPolicy p;
        p.name      = cmd.policyName;
        p.table     = cmd.tableName;
        p.command   = cmd.policyCommand.empty() ? "ALL" : cmd.policyCommand;
        p.role      = cmd.policyUser.empty()    ? "PUBLIC" : cmd.policyUser;
        p.usingExpr = cmd.policyUsingExpr;
        engine.createRlsPolicy(p);
        std::cout << "  Policy " << p.name << " created.\n\n";
        break;
    }
    case milansql::CommandType::DROP_POLICY:
        engine.dropRlsPolicy(cmd.policyName, cmd.tableName);
        std::cout << "  Policy " << cmd.policyName << " dropped.\n\n";
        break;
    case milansql::CommandType::SHOW_POLICIES_ON:
        engine.showPolicies(cmd.tableName);
        std::cout << "\n";
        break;

    // ── Phase 76: LISTEN / NOTIFY / UNLISTEN ─────────────────────
    case milansql::CommandType::LISTEN: {
        if (cmd.channelName.empty()) {
            std::cout << "  Fehler: LISTEN braucht einen Channel-Namen.\n\n";
            break;
        }
        g_pubsub().listen(cmd.channelName, engine.getCurrentUser());
        std::cout << "  Listening on channel '" << cmd.channelName << "'.\n\n";
        break;
    }
    case milansql::CommandType::UNLISTEN: {
        if (cmd.channelName == "*") {
            g_pubsub().unlistenAll(engine.getCurrentUser());
            std::cout << "  Unlistened all channels.\n\n";
        } else if (cmd.channelName.empty()) {
            std::cout << "  Fehler: UNLISTEN braucht einen Channel-Namen oder '*'.\n\n";
        } else {
            g_pubsub().unlisten(cmd.channelName, engine.getCurrentUser());
            std::cout << "  Unlistened channel '" << cmd.channelName << "'.\n\n";
        }
        break;
    }
    case milansql::CommandType::NOTIFY: {
        if (cmd.channelName.empty()) {
            std::cout << "  Fehler: NOTIFY braucht einen Channel-Namen.\n\n";
            break;
        }
        size_t n = g_pubsub().notify(cmd.channelName, cmd.notifyPayload);
        std::cout << "  Notification sent on '" << cmd.channelName << "'";
        if (!cmd.notifyPayload.empty())
            std::cout << " payload='" << cmd.notifyPayload << "'";
        std::cout << "  (" << n << " listener(s) notified).\n\n";
        // Drain own notifications (single-session REPL: print them inline)
        auto msgs = g_pubsub().pending(engine.getCurrentUser());
        for (const auto& m : msgs) {
            std::cout << "  NOTIFY " << m.channel;
            if (!m.payload.empty()) std::cout << " '" << m.payload << "'";
            std::cout << "\n";
        }
        if (!msgs.empty()) std::cout << "\n";
        break;
    }
    case milansql::CommandType::SHOW_LISTEN: {
        auto chans = g_pubsub().activeChannels(engine.getCurrentUser());
        if (chans.empty()) {
            std::cout << "  Not listening on any channels.\n\n";
        } else {
            std::cout << "  Listening on: ";
            for (size_t i = 0; i < chans.size(); ++i) {
                if (i) std::cout << ", ";
                std::cout << chans[i];
            }
            std::cout << "\n\n";
        }
        break;
    }

    // ── Phase 78: Table Inheritance ──────────────────────────────
    case milansql::CommandType::SHOW_INHERITANCE:
        engine.showInheritance();
        break;

    // ── Phase 80: Column Store Engine ────────────────────────────
    case milansql::CommandType::SHOW_STORAGE_FORMAT:
        engine.showStorageFormat();
        break;

    // ── Phase 77: Parallel Query ──────────────────────────────────
    case milansql::CommandType::SHOW_PARALLEL_STATUS:
        engine.showParallelStatus();
        break;

    case milansql::CommandType::SET_PARALLEL_THRESHOLD: {
        if (!cmd.values.empty()) {
            try {
                long long t = std::stoll(cmd.values[0]);
                engine.setParallelThreshold(t);
                std::cout << "  PARALLEL_THRESHOLD = " << t << " rows.\n\n";
            } catch (...) {
                std::cout << "  Fehler: SET PARALLEL_THRESHOLD = <N>\n\n";
            }
        }
        break;
    }

    case milansql::CommandType::SET_MAX_PARALLEL_WORKERS: {
        if (!cmd.values.empty()) {
            try {
                int n = std::stoi(cmd.values[0]);
                engine.setMaxParallelWorkers(n);
                std::cout << "  MAX_PARALLEL_WORKERS = " << n << " threads.\n\n";
            } catch (...) {
                std::cout << "  Fehler: SET MAX_PARALLEL_WORKERS = <N>\n\n";
            }
        }
        break;
    }

    case milansql::CommandType::CREATE_PUBLICATION: {
        bool allTables = (!cmd.values.empty() && cmd.values[0] == "*");
        std::vector<std::string> tables;
        if (!allTables) tables = cmd.values;
        engine.createPublication(cmd.tableName, tables, allTables);
        if (allTables)
            std::cout << "  Publication '" << cmd.tableName << "' erstellt (ALL TABLES).\n\n";
        else
            std::cout << "  Publication '" << cmd.tableName << "' erstellt ("
                      << tables.size() << " Tabelle(n)).\n\n";
        break;
    }
    case milansql::CommandType::DROP_PUBLICATION:
        engine.dropPublication(cmd.tableName);
        std::cout << "  Publication '" << cmd.tableName << "' geloescht.\n\n";
        break;
    case milansql::CommandType::SHOW_PUBLICATIONS:
        engine.showPublications();
        break;
    case milansql::CommandType::CREATE_SUBSCRIPTION: {
        std::string conn, pub;
        auto sep = cmd.viewSql.find('\x01');
        if (sep != std::string::npos) {
            conn = cmd.viewSql.substr(0, sep);
            pub  = cmd.viewSql.substr(sep + 1);
        }
        engine.createSubscription(cmd.tableName, conn, pub);
        std::cout << "  Subscription '" << cmd.tableName << "' erstellt ("
                  << conn << " -> " << pub << ").\n\n";
        break;
    }
    case milansql::CommandType::DROP_SUBSCRIPTION:
        engine.dropSubscription(cmd.tableName);
        std::cout << "  Subscription '" << cmd.tableName << "' geloescht.\n\n";
        break;
    case milansql::CommandType::SHOW_SUBSCRIPTIONS:
        engine.showSubscriptions();
        break;
    case milansql::CommandType::ALTER_SUBSCRIPTION: {
        bool enable = (cmd.alterOp == "ENABLE");
        engine.alterSubscription(cmd.tableName, enable);
        std::cout << "  Subscription '" << cmd.tableName << "' "
                  << (enable ? "aktiviert" : "deaktiviert") << ".\n\n";
        break;
    }
    case milansql::CommandType::SHOW_LOGICAL_LOG:
        engine.showLogicalLog();
        break;

    // ── Phase 82: Adaptive Query Optimizer ───────────────────────

    case milansql::CommandType::SHOW_QUERY_STATS:
        g_adaptiveStats.showStats();
        break;

    case milansql::CommandType::SHOW_INDEX_SUGGESTIONS:
        g_adaptiveStats.showIndexSuggestions();
        break;

    case milansql::CommandType::ANALYZE_TABLE: {
        if (cmd.tableName.empty()) {
            std::cout << "  Fehler: ANALYZE TABLE tabellenname\n\n"; break;
        }
        if (cmd.tableName == "*") {
            // ANALYZE — all tables
            const auto& tables = engine.getTables();
            size_t count = 0;
            for (const auto& kv : tables) {
                g_tableStats.analyzeTable(kv.first, kv.second);
                g_adaptiveStats.analyzeTable(kv.first);
                ++count;
            }
            g_tableStats.saveStats();
            g_adaptiveStats.saveStats();
            std::cout << "  ANALYZE: " << count << " Tabelle(n) analysiert.\n\n";
        } else {
            // ANALYZE TABLE name
            g_adaptiveStats.analyzeTable(cmd.tableName);
            if (engine.tableExists(cmd.tableName)) {
                const Table& tbl = engine.getTables().at(cmd.tableName);
                g_tableStats.analyzeTable(cmd.tableName, tbl);
                g_tableStats.saveStats();
                std::cout << "  ANALYZE TABLE '" << cmd.tableName
                          << "': " << tbl.rowCount() << " Zeile(n), "
                          << tbl.columns().size() << " Spalte(n) analysiert.\n\n";
            } else {
                std::cout << "  ANALYZE TABLE '" << cmd.tableName
                          << "': Statistiken zurueckgesetzt.\n\n";
            }
            g_adaptiveStats.saveStats();
        }
        break;
    }

    // ── Phase 86: SHOW STATISTICS FOR <table> ────────────────
    case milansql::CommandType::SHOW_STATISTICS_FOR: {
        if (cmd.tableName.empty()) {
            std::cout << "  Fehler: SHOW STATISTICS FOR tabellenname\n\n"; break;
        }
        g_tableStats.showStatistics(cmd.tableName);
        break;
    }

    // ── Phase 87: SET RECURSIVE_MAX_ITERATIONS = N ───────────
    case milansql::CommandType::SET_RECURSIVE_MAX_ITERATIONS: {
        std::string val = cmd.values.empty() ? "" : cmd.values[0];
        try {
            int n = std::stoi(val);
            if (n < 1) n = 1;
            g_recursiveMaxIter = n;
            std::cout << "  RECURSIVE_MAX_ITERATIONS gesetzt: " << n << "\n\n";
        } catch (...) {
            std::cout << "  Fehler: SET RECURSIVE_MAX_ITERATIONS = <zahl>\n\n";
        }
        break;
    }

    // ── Phase 89: Foreign Data Wrapper ───────────────────────────

    case milansql::CommandType::CREATE_SERVER: {
        if (cmd.serverName.empty() || cmd.wrapperType.empty()) {
            std::cout << "  Fehler: CREATE SERVER name FOREIGN DATA WRAPPER type\n\n"; break;
        }
        engine.createServer(cmd.serverName, cmd.wrapperType);
        std::cout << "  Server '" << cmd.serverName
                  << "' erstellt (FOREIGN DATA WRAPPER: " << cmd.wrapperType << ").\n\n";
        break;
    }

    case milansql::CommandType::DROP_SERVER: {
        if (cmd.serverName.empty()) {
            std::cout << "  Fehler: DROP SERVER name\n\n"; break;
        }
        engine.dropServer(cmd.serverName);
        std::cout << "  Server '" << cmd.serverName << "' geloescht.\n\n";
        break;
    }

    case milansql::CommandType::SHOW_SERVERS:
        engine.showServers();
        break;

    case milansql::CommandType::CREATE_FOREIGN_TABLE: {
        if (cmd.tableName.empty() || cmd.serverName.empty()) {
            std::cout << "  Fehler: CREATE FOREIGN TABLE name (...) SERVER server OPTIONS (...)\n\n"; break;
        }
        milansql::ForeignTableDef ftd;
        ftd.name = cmd.tableName;
        ftd.serverName = cmd.serverName;
        for (const auto& col : cmd.columns) {
            ftd.colNames.push_back(col.name);
            ftd.colTypes.push_back(col.type);
        }
        ftd.options = cmd.fdwOptions;
        engine.createForeignTable(std::move(ftd));
        std::cout << "  Foreign Table '" << cmd.tableName
                  << "' erstellt (SERVER: " << cmd.serverName << ", "
                  << cmd.columns.size() << " Spalte(n)).\n\n";
        break;
    }

    case milansql::CommandType::DROP_FOREIGN_TABLE: {
        if (cmd.tableName.empty()) {
            std::cout << "  Fehler: DROP FOREIGN TABLE name\n\n"; break;
        }
        engine.dropForeignTable(cmd.tableName);
        std::cout << "  Foreign Table '" << cmd.tableName << "' geloescht.\n\n";
        break;
    }

    case milansql::CommandType::SHOW_FOREIGN_TABLES:
        engine.showForeignTables();
        break;

    case milansql::CommandType::SET_QUERY_REWRITE: {
        if (cmd.queryRewriteFlag == "ON") {
            g_queryRewriter.setEnabled(true);
            std::cout << "  Query Rewriter aktiviert.\n\n";
        } else if (cmd.queryRewriteFlag == "OFF") {
            g_queryRewriter.setEnabled(false);
            std::cout << "  Query Rewriter deaktiviert.\n\n";
        } else {
            std::cout << "  Fehler: SET QUERY_REWRITE = ON|OFF\n\n";
        }
        break;
    }

    case milansql::CommandType::EXPLAIN_REWRITTEN: {
        if (cmd.benchmarkSql.empty()) {
            std::cout << "  Fehler: EXPLAIN REWRITTEN SELECT ...\n\n"; break;
        }
        // Parse inner query
        milansql::ParsedCommand inner = parser.parse(cmd.benchmarkSql);
        // Resolve subqueries
        for (const auto& sq : inner.subqueries) {
            if (sq.condIdx < inner.whereConds.size())
                inner.whereConds[sq.condIdx].inList =
                    engine.subqueryValues(sq.subTable, sq.subCol,
                                          sq.subWhere, sq.subWhereLogic);
        }
        // Apply rewriter (temp enable if disabled)
        bool wasEnabled = g_queryRewriter.isEnabled();
        g_queryRewriter.setEnabled(true);
        bool changed = g_queryRewriter.rewrite(inner);
        g_queryRewriter.setEnabled(wasEnabled);

        std::cout << "\n  Original Query:\n    " << cmd.benchmarkSql << "\n\n";

        if (!g_queryRewriter.notes().empty()) {
            std::cout << "  Umschreibungen:\n";
            for (const auto& n : g_queryRewriter.notes())
                std::cout << "    * " << n << "\n";
            std::cout << "\n";
        } else {
            std::cout << "  Umschreibungen: (keine angewendet)\n\n";
        }
        (void)changed;

        std::cout << "  Ausfuehrungsplan:\n";
        std::cout << "    Tabelle  : " << (inner.tableName.empty() ? "(unbekannt)" : inner.tableName) << "\n";
        if (inner.isJoin)
            std::cout << "    Operation: JOIN (" << inner.joinClauses.size() << " Join(s))\n";
        else if (inner.isGroupBy)
            std::cout << "    Operation: GROUP BY " << (inner.groupByCols.empty() ? "" : inner.groupByCols[0]) << "\n";
        else if (inner.isAggregate)
            std::cout << "    Operation: " << inner.aggFunc << "(" << inner.aggCol << ")\n";
        else if (inner.isCount)
            std::cout << "    Operation: COUNT(*)\n";
        else
            std::cout << "    Operation: SELECT\n";
        if (!inner.whereConds.empty())
            std::cout << "    WHERE    : " << dispatch_whereDesc(inner) << "\n";
        std::cout << "    Scan     : " << (inner.whereConds.empty() ? "FULL TABLE SCAN" : "FULL SCAN mit Filter") << "\n";
        if (inner.limit >= 0)
            std::cout << "    LIMIT    : " << inner.limit << "\n";
        std::cout << "\n";
        break;
    }

    // ── Phase 90: Extension System ────────────────────────────
    case milansql::CommandType::CREATE_EXTENSION: {
        bool ok = engine.getExtensionManager().createExtension(cmd.extensionName);
        if (ok)
            std::cout << "  Extension '" << cmd.extensionName << "' loaded.\n\n";
        else
            std::cout << "  FEHLER: Unknown extension '" << cmd.extensionName
                      << "'. Available: milansql_math, milansql_crypto, milansql_uuid, milansql_text\n\n";
        break;
    }

    case milansql::CommandType::DROP_EXTENSION: {
        engine.getExtensionManager().dropExtension(cmd.extensionName);
        std::cout << "  Extension '" << cmd.extensionName << "' dropped.\n\n";
        break;
    }

    case milansql::CommandType::SHOW_EXTENSIONS: {
        std::string list = engine.getExtensionManager().showExtensions();
        if (list == "(no extensions loaded)") {
            std::cout << "  " << list << "\n\n";
        } else {
            std::cout << "\n  Loaded extensions:\n";
            std::istringstream iss(list);
            std::string line;
            while (std::getline(iss, line))
                std::cout << "    - " << line << "\n";
            std::cout << "\n";
        }
        break;
    }

    // ── Phase 92: COPY FROM ────────────────────────────────────
    case milansql::CommandType::COPY_FROM: {
        if (dispatch_slaveReadOnly()) {
            std::cout << "  FEHLER: Read-only: Slave akzeptiert keine Schreiboperationen\n\n";
            break;
        }
        try {
            if (cmd.copyStdin) {
                // STDIN is handled in main.cpp before dispatch; should not reach here
                std::cout << "  FEHLER: COPY FROM STDIN muss im REPL-Modus ausgefuehrt werden.\n\n";
            } else if (cmd.copyFormat == "BINARY") {
                std::string result = g_copyManager.copyFrom(engine, cmd.tableName,
                    cmd.copyFile, "BINARY", cmd.copyDelimiter, cmd.copyHeader);
                std::cout << "  " << result << "\n\n";
                persistFn();
            } else {
                std::string result = g_copyManager.copyFrom(engine, cmd.tableName,
                    cmd.copyFile, "CSV", cmd.copyDelimiter, cmd.copyHeader);
                std::cout << "  " << result << "\n\n";
                persistFn();
            }
        } catch (const std::exception& ex) {
            std::cout << "  FEHLER (COPY FROM): " << ex.what() << "\n\n";
        }
        break;
    }

    // ── Phase 92: COPY TO ──────────────────────────────────────
    case milansql::CommandType::COPY_TO: {
        try {
            if (!cmd.copySubquery.empty()) {
                // COPY (SELECT ...) TO 'file'
                milansql::ParsedCommand subCmd = parser.parse(cmd.copySubquery);
                milansql::Table subResult = dispatch_executeSelectToTable(engine, parser, subCmd);
                // Build column names
                std::vector<std::string> colNames;
                for (const auto& c : subResult.columns()) colNames.push_back(c.name);
                // Build rows
                std::vector<std::vector<std::string>> rowData;
                for (const auto& r : subResult.rows()) rowData.push_back(r.values);
                std::string result = g_copyManager.copyQueryTo(colNames, rowData,
                    cmd.copyFile, cmd.copyDelimiter, cmd.copyHeader);
                std::cout << "  " << result << "\n\n";
            } else if (cmd.copyFormat == "BINARY") {
                std::string result = g_copyManager.copyTo(engine, cmd.tableName,
                    cmd.copyFile, "BINARY", cmd.copyDelimiter, cmd.copyHeader);
                std::cout << "  " << result << "\n\n";
            } else {
                std::string result = g_copyManager.copyTo(engine, cmd.tableName,
                    cmd.copyFile, "CSV", cmd.copyDelimiter, cmd.copyHeader);
                std::cout << "  " << result << "\n\n";
            }
        } catch (const std::exception& ex) {
            std::cout << "  FEHLER (COPY TO): " << ex.what() << "\n\n";
        }
        break;
    }

    // ── Phase 92: SHOW COPY STATS ──────────────────────────────
    case milansql::CommandType::SHOW_COPY_STATS: {
        std::string stats = g_copyManager.showStats();
        std::cout << "\n  " << stats << "\n\n";
        break;
    }

    case milansql::CommandType::UNKNOWN:
    default:
        std::cout << "  Unbekannter Befehl: '" << eingabe
                  << "'\n  Tippe 'help' fuer eine Uebersicht.\n\n";
        break;
    }

    return false;
}

} // namespace milansql
