#pragma once
// ============================================================
// dispatch.hpp — Shared SQL command dispatch for MilanSQL
// Used by main.cpp (REPL) and server.hpp (TCP Server)
// Phase 47: extracted from main.cpp
// ============================================================

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>

#include "engine/engine.hpp"
#include "engine/btree.hpp"
#include "parser/parser.hpp"
#include "storage/storage.hpp"

namespace milansql {

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
            widths[i] = std::max(widths[i], rows[r].values[i].size());

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
                const std::string& val =
                    (i < rows[r].values.size()) ? rows[r].values[i] : "";
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

    std::vector<std::string> hdr = {"Name", "Typ", "NOT NULL", "UNIQUE", "DEFAULT", "PK", "AI", "CHECK"};
    std::vector<size_t> w(8);
    for (size_t i = 0; i < 8; ++i) w[i] = hdr[i].size();
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
    }

    auto hline = [&](const std::string& l, const std::string& m,
                     const std::string& r, const std::string& f) {
        std::cout << l;
        for (size_t i = 0; i < 8; ++i) {
            for (size_t j = 0; j < w[i] + 2; ++j) std::cout << f;
            if (i + 1 < 8) std::cout << m;
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
    for (size_t i = 0; i < 8; ++i) cell(hdr[i], w[i]);
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
        std::cout << "  \u2502";
        cell(col.name,                               w[0]);
        cell(col.type,                               w[1]);
        cell(col.notNull      ? "YES" : "NO",        w[2]);
        cell(col.isUnique     ? "YES" : "NO",        w[3]);
        cell(col.hasDefault   ? col.defaultValue:"-",w[4]);
        cell(col.isPrimaryKey ? "YES" : "NO",        w[5]);
        cell(col.autoIncrement? "YES" : "NO",        w[6]);
        cell(checkStr,                               w[7]);
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
        auto result = engine.executeJoins(
            cmd.tableName, cmd.joinClauses,
            cmd.whereConds, cmd.whereLogic);
        if (!cmd.orderByCols.empty()) result.sortByMulti(cmd.orderByCols);
        if (!cmd.selectColumns.empty())
            result = result.project(cmd.selectColumns);
        return result;
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

    milansql::Table result;
    if (!cmd.whereConds.empty()) {
        auto qr = engine.selectWhere(cmd.tableName, cmd.whereConds, cmd.whereLogic);
        result = std::move(qr.table);
    } else {
        result = engine.selectAll(cmd.tableName).clone();
    }

    if (cmd.hasCaseItems && !cmd.selectItems.empty()) {
        bool hasWin = false;
        for (const auto& si : cmd.selectItems)
            if (si.isWindowFunc) { hasWin = true; break; }
        if (hasWin)
            result = engine.projectWithWindowItems(result, cmd.selectItems);
        else
            result = engine.projectWithItems(result, cmd.selectItems);
        if (!cmd.orderByCols.empty()) result.sortByMulti(cmd.orderByCols);
    } else {
        if (!cmd.orderByCols.empty()) result.sortByMulti(cmd.orderByCols);
        if (!cmd.selectColumns.empty())
            result = result.project(cmd.selectColumns);
    }
    if (cmd.isDistinct) result.makeDistinct();
    return result;
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
    std::function<void()> saveTriggFn)
{
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
        dispatch_printIndexes(engine.getIndexes(cmd.tableName), cmd.tableName);
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

    case milansql::CommandType::CREATE_TABLE: {
        if (cmd.tableName.empty() || cmd.columns.empty()) {
            std::cout << "  Fehler: CREATE TABLE name (col TYP, ...)\n"; break;
        }
        engine.createTable(cmd.tableName, cmd.columns, cmd.foreignKeys);
        persistFn();
        std::cout << "  Tabelle '" << cmd.tableName << "' erstellt ("
                  << cmd.columns.size() << " Spalten";
        if (!cmd.foreignKeys.empty())
            std::cout << ", " << cmd.foreignKeys.size() << " FK";
        std::cout << ").\n\n";
        break;
    }

    case milansql::CommandType::INSERT: {
        if (cmd.tableName.empty()) {
            std::cout << "  Fehler: Kein Tabellenname.\n"; break;
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
            std::cout << "  " << count << " Zeile(n) eingefuegt in '"
                      << cmd.tableName << "' (INSERT INTO SELECT).\n\n";
            break;
        }

        const auto& rows = cmd.multiValues.empty()
            ? std::vector<std::vector<std::string>>{cmd.values}
            : cmd.multiValues;

        if (cmd.upsertMode == "REPLACE") {
            size_t replaced = 0, inserted = 0;
            for (const auto& vals : rows)
                engine.insertOrReplace(cmd.tableName, vals) ? ++replaced : ++inserted;
            persistFn();
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
            if (rows.size() == 1)
                std::cout << "  1 Zeile eingefuegt in '" << cmd.tableName << "'.\n\n";
            else
                std::cout << "  " << rows.size() << " Zeilen eingefuegt in '"
                          << cmd.tableName << "'.\n\n";
        }
        break;
    }

    case milansql::CommandType::SELECT: {
        if (cmd.tableName.empty()) {
            std::cout << "  Fehler: Kein Tabellenname.\n"; break;
        }
        if (!engine.viewExists(cmd.tableName) && cmd.cteList.empty()) {
            engine.checkPrivilege("SELECT", cmd.tableName);
        }

        if (!cmd.cteList.empty()) {
            try {
                for (auto& [cteName, cteInnerSql] : cmd.cteList) {
                    milansql::ParsedCommand cteParsed = parser.parse(cteInnerSql);
                    milansql::Table cteResult =
                        dispatch_executeSelectToTable(engine, parser, cteParsed);
                    engine.registerTempTable(cteName, std::move(cteResult));
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

        if (cmd.isExplain) {
            milansql::ExplainRequest req;
            req.tableName     = cmd.tableName;
            req.isJoin        = cmd.isJoin;
            req.joinClauses   = cmd.joinClauses;
            req.whereConds    = cmd.whereConds;
            req.whereLogic    = cmd.whereLogic;
            req.isGroupBy     = cmd.isGroupBy;
            req.groupByCols   = cmd.groupByCols;
            req.havingConds   = cmd.havingConds;
            req.isAggregate   = cmd.isAggregate;
            req.aggFunc       = cmd.aggFunc;
            req.aggCol        = cmd.aggCol;
            req.selectItems   = cmd.selectItems;
            req.hasCaseItems  = cmd.hasCaseItems;
            req.selectColumns = cmd.selectColumns;
            req.orderByCols   = cmd.orderByCols;
            req.limit         = cmd.limit;
            req.limitOffset   = cmd.limitOffset;
            req.isSetOp       = cmd.isSetOp;
            req.setOp         = cmd.setOp;
            dispatch_printExplain(engine.buildExplain(req));
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

        if (cmd.isJoin) {
            if (cmd.joinClauses.empty()) {
                std::cout << "  Fehler: JOIN-Syntax: "
                             "FROM t1 [LEFT|INNER] JOIN t2 ON t1.col = t2.col\n";
                break;
            }
            auto result = engine.executeJoins(
                cmd.tableName, cmd.joinClauses,
                cmd.whereConds, cmd.whereLogic);
            std::cout << "\n";
            if (!cmd.orderByCols.empty())
                result.sortByMulti(cmd.orderByCols);
            if (!cmd.selectColumns.empty())
                result = result.project(cmd.selectColumns);
            dispatch_printTable(result, cmd.limit, cmd.limitOffset);
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
                cmd.tableName, cmd.whereConds, cmd.whereLogic);
            std::cout << "\n  COUNT(*) = " << n
                      << " (Tabelle '" << cmd.tableName << "')";
            if (!cmd.whereConds.empty())
                std::cout << "  [WHERE " << dispatch_whereDesc(cmd) << "]";
            std::cout << "\n\n";
            break;
        }

        std::cout << "\n";

        milansql::Table result;
        bool usedIndex = false;

        if (!cmd.whereConds.empty()) {
            auto qr = engine.selectWhere(
                cmd.tableName, cmd.whereConds, cmd.whereLogic);
            usedIndex = qr.usedIndex;
            result    = std::move(qr.table);
        } else {
            result = engine.selectAll(cmd.tableName).clone();
        }

        std::size_t totalFound = result.rowCount();

        if (totalFound == 0 && !cmd.whereConds.empty()) {
            std::string lbl = usedIndex ? "[INDEX SCAN]" : "[FULL SCAN] ";
            std::cout << "  " << lbl << " 0 Zeilen gefunden (WHERE "
                      << dispatch_whereDesc(cmd) << ")\n\n";
            break;
        }

        if (cmd.hasCaseItems && !cmd.selectItems.empty()) {
            bool hasWin = false;
            for (const auto& si : cmd.selectItems)
                if (si.isWindowFunc) { hasWin = true; break; }
            if (hasWin)
                result = engine.projectWithWindowItems(result, cmd.selectItems);
            else
                result = engine.projectWithItems(result, cmd.selectItems);
            if (!cmd.orderByCols.empty())
                result.sortByMulti(cmd.orderByCols);
        } else {
            if (!cmd.orderByCols.empty())
                result.sortByMulti(cmd.orderByCols);
            if (!cmd.selectColumns.empty())
                result = result.project(cmd.selectColumns);
        }
        if (cmd.isDistinct)
            result.makeDistinct();

        dispatch_printTable(result, cmd.limit, cmd.limitOffset);

        if (!cmd.whereConds.empty()) {
            std::string lbl = usedIndex ? "[INDEX SCAN]" : "[FULL SCAN] ";
            std::cout << "  " << lbl << " " << totalFound
                      << " Zeile(n) (WHERE " << dispatch_whereDesc(cmd) << ")\n\n";
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
            std::cout << "  " << n << " Zeile(n) aktualisiert"
                      << " (SET " << setDesc << ")\n\n";
        } else {
            std::size_t n = engine.updateWhere(
                cmd.tableName,
                cmd.updateCols, cmd.updateVals,
                cmd.whereColumn, cmd.whereValue);
            if (n > 0) persistFn();
            std::cout << "  " << n << " Zeile(n) aktualisiert"
                      << " (SET " << setDesc
                      << " WHERE " << cmd.whereColumn
                      << " = " << cmd.whereValue << ")\n\n";
        }
        break;
    }

    case milansql::CommandType::TRUNCATE: {
        if (cmd.tableName.empty()) {
            std::cout << "  Fehler: TRUNCATE TABLE tabellenname\n"; break;
        }
        engine.truncateTable(cmd.tableName);
        persistFn();
        std::cout << "  Tabelle '" << cmd.tableName
                  << "' geleert (Schema + Constraints behalten,"
                  << " AUTO_INCREMENT = 1).\n\n";
        break;
    }

    case milansql::CommandType::DELETE: {
        if (cmd.tableName.empty()) {
            std::cout << "  Fehler: DELETE FROM tabelle [WHERE ...]\n"; break;
        }
        if (cmd.whereColumn.empty()) {
            std::size_t n = engine.deleteAll(cmd.tableName);
            persistFn();
            std::cout << "  " << n << " Zeile(n) geloescht.\n\n";
        } else {
            std::size_t n = engine.deleteWhere(
                cmd.tableName, cmd.whereColumn, cmd.whereValue);
            if (n > 0) persistFn();
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
        struct KV { std::string key, val; };
        std::vector<KV> kvs = {
            {"Datei",          "database.milan"},
            {"Format-Version", std::to_string(milansql::MilanBinaryStorage::FORMAT_VERSION)},
            {"Tabellen",       std::to_string(tnames.size())},
            {"Views",          std::to_string(vnames.size())},
            {"Gesamt-Spalten", std::to_string(totalCols)},
            {"Gesamt-Zeilen",  std::to_string(totalRows)},
            {"Datei-Groesse",  fileSize},
        };
        size_t maxKey = 0, maxVal = 0;
        for (const auto& kv : kvs) {
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
        if (cmd.tableName.empty()) {
            std::cout << "  Fehler: Kein Tabellenname.\n"; break;
        }
        engine.dropTable(cmd.tableName);
        persistFn();
        std::cout << "  Tabelle '" << cmd.tableName << "' geloescht.\n\n";
        break;
    }

    case milansql::CommandType::ALTER_TABLE: {
        if (cmd.tableName.empty() || cmd.alterOp.empty()) {
            std::cout << "  Fehler: ALTER TABLE name ADD|DROP|RENAME COLUMN ...\n";
            break;
        }
        engine.alterTable(cmd.tableName, cmd.alterOp,
                          cmd.alterColName, cmd.alterColType,
                          cmd.alterColNew);
        persistFn();
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

    case milansql::CommandType::BEGIN:
        engine.beginTransaction("database.milan.wal");
        std::cout << "  Transaktion gestartet.\n\n";
        break;

    case milansql::CommandType::COMMIT:
        engine.applyAndCommit();
        persistFn();
        std::cout << "  Transaktion erfolgreich abgeschlossen (COMMIT).\n\n";
        break;

    case milansql::CommandType::ROLLBACK:
        engine.rollbackTransaction();
        std::cout << "  Transaktion abgebrochen (ROLLBACK).\n\n";
        break;

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
        std::string body = engine.getProcedureBody(
            cmd.procedureName, cmd.callArgs);
        std::vector<std::string> stmts;
        std::string cur;
        for (char c : body) {
            if (c == ';') {
                while (!cur.empty() &&
                       (cur.front()==' '||cur.front()=='\n'||cur.front()=='\t'))
                    cur = cur.substr(1);
                while (!cur.empty() &&
                       (cur.back()==' '||cur.back()=='\n'||cur.back()=='\t'))
                    cur.pop_back();
                if (!cur.empty()) stmts.push_back(cur);
                cur = "";
            } else cur += c;
        }
        while (!cur.empty() &&
               (cur.front()==' '||cur.front()=='\n'||cur.front()=='\t'))
            cur = cur.substr(1);
        while (!cur.empty() &&
               (cur.back()==' '||cur.back()=='\n'||cur.back()=='\t'))
            cur.pop_back();
        if (!cur.empty()) stmts.push_back(cur);

        for (const auto& stmt : stmts) {
            if (stmt.empty()) continue;
            milansql::Parser stmtParser;
            milansql::ParsedCommand sc = stmtParser.parse(stmt);
            for (const auto& sq : sc.subqueries) {
                if (sq.condIdx < sc.whereConds.size()) {
                    sc.whereConds[sq.condIdx].inList =
                        engine.subqueryValues(sq.subTable, sq.subCol,
                                              sq.subWhere, sq.subWhereLogic);
                }
            }
            try {
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
                    const auto& rows44 = sc.multiValues.empty()
                        ? std::vector<std::vector<std::string>>{sc.values}
                        : sc.multiValues;
                    for (const auto& vals : rows44)
                        engine.insertRow(sc.tableName, vals);
                    persistFn();
                    std::cout << "  " << rows44.size()
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
                    std::cout << "  [CALL] Unbekannter Befehl in Procedure: '"
                              << stmt << "'\n\n";
                }
            } catch (const std::exception& ex2) {
                std::cout << "  FEHLER in Procedure: " << ex2.what() << "\n\n";
            }
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

    case milansql::CommandType::UNKNOWN:
    default:
        std::cout << "  Unbekannter Befehl: '" << eingabe
                  << "'\n  Tippe 'help' fuer eine Uebersicht.\n\n";
        break;
    }

    return false;
}

} // namespace milansql
