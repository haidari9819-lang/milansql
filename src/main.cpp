#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>

#include "engine/engine.hpp"
#include "engine/btree.hpp"
#include "parser/parser.hpp"
#include "storage/storage.hpp"

// ============================================================
// main.cpp — REPL für MilanSQL (Phase 32)
// Neu: String-Funktionen in SELECT (UPPER/LOWER/LENGTH/CONCAT/SUBSTR/TRIM/REPLACE)
// ============================================================

static void printTable(const milansql::Table& tbl, int limit = -1, int offset = 0) {
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

    hline("┌", "┬", "┐", "─");
    std::cout << "│";
    for (size_t i = 0; i < cols.size(); ++i) {
        std::cout << " " << cols[i].name;
        for (size_t j = cols[i].name.size(); j < widths[i]; ++j) std::cout << " ";
        std::cout << " │";
    }
    std::cout << "\n";
    hline("├", "┼", "┤", "─");

    if (printRows == 0) {
        std::cout << "│";
        for (size_t i = 0; i < cols.size(); ++i)
            std::cout << "  " << std::string(widths[i], ' ') << "│";
        std::cout << "\n";
    } else {
        for (size_t r = startRow; r < startRow + printRows; ++r) {
            std::cout << "│";
            for (size_t i = 0; i < cols.size(); ++i) {
                const std::string& val =
                    (i < rows[r].values.size()) ? rows[r].values[i] : "";
                std::cout << " " << val;
                for (size_t j = val.size(); j < widths[i]; ++j) std::cout << " ";
                std::cout << " │";
            }
            std::cout << "\n";
        }
    }
    hline("└", "┴", "┘", "─");

    if (limit >= 0 || offset > 0) {
        std::cout << "  " << printRows << " von " << total << " Zeile(n)";
        if (offset > 0) std::cout << " (OFFSET " << startRow << ")";
        if (limit >= 0) std::cout << " (LIMIT " << limit << ")";
        std::cout << "\n\n";
    } else {
        std::cout << "  " << printRows << " Zeile(n)\n\n";
    }
}

static void printIndexes(const std::vector<milansql::IndexInfo>& indexes,
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
    hline("┌", "┬", "┐", "─");
    std::cout << "│";
    for (size_t i = 0; i < 3; ++i) {
        std::cout << " " << hdr[i];
        for (size_t j = hdr[i].size(); j < w[i]; ++j) std::cout << " ";
        std::cout << " │";
    }
    std::cout << "\n";
    hline("├", "┼", "┤", "─");
    for (const auto& idx : indexes) {
        std::vector<std::string> row = {idx.indexName, idx.colName, idx.type};
        std::cout << "│";
        for (size_t i = 0; i < 3; ++i) {
            std::cout << " " << row[i];
            for (size_t j = row[i].size(); j < w[i]; ++j) std::cout << " ";
            std::cout << " │";
        }
        std::cout << "\n";
    }
    hline("└", "┴", "┘", "─");
    std::cout << "  " << indexes.size() << " Index(e) auf '" << tableName << "'\n\n";
}

// Phase 36: EXPLAIN-Plan als formatierte Tabelle ausgeben
static void printExplain(const milansql::ExplainPlan& plan) {
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

// WHERE-Bedingungen als lesbaren String zusammenbauen
static std::string whereDesc(const milansql::ParsedCommand& cmd) {
    std::string s;
    for (size_t k = 0; k < cmd.whereConds.size(); ++k) {
        if (k > 0) s += " " + cmd.whereLogic + " ";
        s += cmd.whereConds[k].col + " " +
             cmd.whereConds[k].op  + " " +
             cmd.whereConds[k].val;
    }
    return s;
}

static void printDescribe(const milansql::Table& tbl) {
    const auto& cols = tbl.columns();
    if (cols.empty()) { std::cout << "  (Keine Spalten)\n\n"; return; }

    // Spaltenbreiten berechnen (Phase 23: +CHECK als 8. Spalte)
    std::vector<std::string> hdr = {"Name", "Typ", "NOT NULL", "UNIQUE", "DEFAULT", "PK", "AI", "CHECK"};
    std::vector<size_t> w(8);
    for (size_t i = 0; i < 8; ++i) w[i] = hdr[i].size();
    for (const auto& col : cols) {
        w[0] = std::max(w[0], col.name.size());
        w[1] = std::max(w[1], col.type.size());
        w[4] = std::max(w[4], col.hasDefault ? col.defaultValue.size() : size_t(1));
        // CHECK-Spalte: alle Bedingungen zusammenfassen
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
        std::cout << " │";
    };

    std::cout << "\n  Tabelle: " << tbl.name() << "\n";
    hline("  ┌", "┬", "┐", "─");
    std::cout << "  │";
    for (size_t i = 0; i < 8; ++i) cell(hdr[i], w[i]);
    std::cout << "\n";
    hline("  ├", "┼", "┤", "─");

    for (const auto& col : cols) {
        // CHECK-String aufbauen
        std::string checkStr = "-";
        if (!col.checks.empty()) {
            checkStr = "";
            for (const auto& cc : col.checks) {
                if (!checkStr.empty()) checkStr += " ";
                checkStr += cc.op + cc.val;
            }
        }
        std::cout << "  │";
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
    hline("  └", "┴", "┘", "─");
    std::cout << "  " << cols.size() << " Spalte(n)\n";

    // ── Foreign Keys ──────────────────────────────────────────
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
        fhline("  ┌", "┬", "┐", "─");
        std::cout << "  │";
        for (size_t i = 0; i < 4; ++i) cell(fhdr[i], fw[i]);
        std::cout << "\n";
        fhline("  ├", "┼", "┤", "─");
        for (const auto& fk : fks) {
            std::cout << "  │";
            cell(fk.fromCol,  fw[0]);
            cell(fk.refTable, fw[1]);
            cell(fk.refCol,   fw[2]);
            cell(fk.onDelete, fw[3]);
            std::cout << "\n";
        }
        fhline("  └", "┴", "┘", "─");
    }
    std::cout << "\n";
}

// ── Phase 25: CREATE TABLE SQL aus Schema rekonstruieren ──────
static std::string buildCreateTableSql(const milansql::Table& tbl) {
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

// ── Phase 24: View materialisieren ────────────────────────────
// Parst und führt das gespeicherte View-SQL aus, gibt result Table zurück.
// Gibt eine leere Table zurück bei Fehler.
static milansql::Table materializeView(
        milansql::Engine& engine,
        milansql::Parser& parser,
        const std::string& viewName,
        const milansql::ParsedCommand& outerCmd)
{
    const std::string& vsql = engine.getViewSql(viewName);
    milansql::ParsedCommand vc = parser.parse(vsql);

    // Subqueries im View-SQL auflösen
    for (const auto& sq : vc.subqueries) {
        if (sq.condIdx < vc.whereConds.size()) {
            vc.whereConds[sq.condIdx].inList =
                engine.subqueryValues(
                    sq.subTable, sq.subCol,
                    sq.subWhere, sq.subWhereLogic);
        }
    }

    // Basis-Ergebnistabelle aus der View-Definition holen
    milansql::Table base;
    if (!vc.whereConds.empty()) {
        auto qr = engine.selectWhere(vc.tableName, vc.whereConds, vc.whereLogic);
        base = std::move(qr.table);
    } else {
        base = engine.selectAll(vc.tableName).clone();
    }

    // Spaltenprojektion aus dem View-SQL anwenden
    if (!vc.selectColumns.empty())
        base = base.project(vc.selectColumns);

    // Äußeres WHERE des Aufrufers anwenden (z. B. SELECT * FROM view WHERE ...)
    if (!outerCmd.whereConds.empty())
        base = engine.filterTable(base, outerCmd.whereConds, outerCmd.whereLogic);

    return base;
}

static void printHelp() {
    std::cout << "\n"
        << "  Verfuegbare Befehle:\n"
        << "  ──────────────────────────────────────────────────────────────────\n"
        << "  CREATE TABLE name (col TYP [NOT NULL] [UNIQUE] [DEFAULT val]\n"
        << "                        [PRIMARY KEY] [AUTO_INCREMENT],\n"
        << "                     FOREIGN KEY (col) REFERENCES tbl(col), ...)\n"
        << "                                            Tabelle mit Constraints/FK erstellen\n"
        << "  DESCRIBE name                             Spaltendefinition anzeigen\n"
        << "  INSERT INTO name VALUES (v1, v2, ...)     Zeile einfuegen\n"
        << "  INSERT INTO name VALUES (...),(...),(...)  Mehrere Zeilen einfuegen\n"
        << "  INSERT INTO name SELECT ... FROM name2    Aus Abfrage einfuegen\n"
        << "  SELECT * FROM name                        Alle Zeilen\n"
        << "  SELECT col1, col2 FROM name               Spaltenauswahl\n"
        << "  SELECT DISTINCT col FROM name             Eindeutige Werte\n"
        << "  SELECT * FROM name WHERE col op val       Filtern (=,!=,<,>,<=,>=)\n"
        << "  SELECT * FROM name WHERE col LIKE pat     Muster (% und _)\n"
        << "  SELECT * FROM name WHERE .. AND .. OR ..  Mehrere Bedingungen\n"
        << "  SELECT * FROM name ORDER BY col [DESC]    Sortieren\n"
        << "  SELECT * FROM name LIMIT N                Erste N Zeilen\n"
        << "  SELECT COUNT(*) FROM name [WHERE ...]     Zeilen zaehlen\n"
        << "  SELECT MIN(col) FROM name [WHERE ...]     Minimum\n"
        << "  SELECT MAX(col) FROM name [WHERE ...]     Maximum\n"
        << "  SELECT AVG(col) FROM name [WHERE ...]     Durchschnitt\n"
        << "  SELECT SUM(col) FROM name [WHERE ...]     Summe\n"
        << "  SELECT col,AGG(c) FROM name GROUP BY col  Gruppieren\n"
        << "  ... GROUP BY col HAVING AGG(c) op val     Gruppen filtern\n"
        << "  SELECT CASE WHEN col op val THEN x ... END AS alias FROM t\n"
        << "  SELECT ... UNION [ALL] SELECT ...            Vereinigung\n"
        << "  SELECT ... INTERSECT SELECT ...             Schnittmenge\n"
        << "  SELECT ... EXCEPT SELECT ...                Differenz\n"
        << "  SELECT cols FROM t1 [INNER|LEFT] JOIN t2 ON t1.c=t2.c\n"
        << "  SELECT cols FROM t1 RIGHT JOIN t2 ON t1.c=t2.c  Alle rechten Zeilen\n"
        << "  SELECT cols FROM t1 FULL [OUTER] JOIN t2 ON ..   Alle Zeilen beider Tabellen\n"
        << "  SELECT cols FROM t1 JOIN t2 ON .. JOIN t3 ON ..  Mehrere JOINs\n"
        << "  SELECT * FROM t WHERE col IS NULL                NULL-Prüfung\n"
        << "  SELECT * FROM t WHERE col IS NOT NULL            Nicht-NULL\n"
        << "  SELECT * FROM t WHERE col IN (v1, v2, ...)       IN-Liste\n"
        << "  SELECT * FROM t WHERE col IN (SELECT c FROM t2)  Subquery\n"
        << "  SELECT * FROM t WHERE col BETWEEN x AND y        Bereich\n"
        << "  SELECT * FROM t WHERE EXISTS (SELECT 1 FROM t2 WHERE ...) EXISTS\n"
        << "  ALTER TABLE name ADD COLUMN col TYP               Spalte hinzufügen\n"
        << "  ALTER TABLE name DROP COLUMN col                  Spalte loeschen\n"
        << "  ALTER TABLE name RENAME COLUMN alt TO neu         Spalte umbenennen\n"
        << "  BEGIN                                     Transaktion starten\n"
        << "  COMMIT                                    Transaktion bestaetigen\n"
        << "  ROLLBACK                                  Transaktion abbrechen\n"
        << "  UPDATE name SET col=val [, col=val ...] [WHERE ...] Zeilen aendern\n"
        << "  TRUNCATE TABLE name                       Alle Zeilen loeschen, Schema behalten\n"
        << "  DELETE FROM name [WHERE col = val]        Zeilen loeschen\n"
        << "  CREATE INDEX name ON table (col)          B-Tree Index erstellen\n"
        << "  DROP INDEX name ON table                  Index loeschen\n"
        << "  CREATE VIEW name AS SELECT ...            View erstellen\n"
        << "  DROP VIEW name                            View loeschen\n"
        << "  SELECT * FROM viewname [WHERE ...]        View abfragen\n"
        << "  DESCRIBE viewname                         View-SQL anzeigen\n"
        << "  SHOW TABLES                               Tabellen/Views tabellarisch\n"
        << "  SHOW CREATE TABLE name                    CREATE TABLE SQL anzeigen\n"
        << "  SHOW INDEXES FROM name                    Indizes anzeigen\n"
        << "  STATUS                                    Datenbank-Statistiken\n"
        << "  DROP TABLE name                           Tabelle loeschen\n"
        << "  help / exit\n"
        << "  ──────────────────────────────────────────────────────────────────\n\n";
}

// ── Phase 41: CTE-Hilfs-Funktion — SELECT → Table (ohne Ausgabe) ──
// Führt ein geparste SELECT-Abfrage aus und gibt das Ergebnis als Table zurück.
// Unterstützt: einfaches SELECT, SET-Operationen (UNION/INTERSECT/EXCEPT), JOIN, GROUP BY.
static milansql::Table executeSelectToTable(
        milansql::Engine& engine,
        milansql::Parser& parser,
        milansql::ParsedCommand cmd)
{
    // Subqueries auflösen
    for (const auto& sq : cmd.subqueries) {
        if (sq.condIdx < cmd.whereConds.size()) {
            cmd.whereConds[sq.condIdx].inList =
                engine.subqueryValues(sq.subTable, sq.subCol,
                                      sq.subWhere, sq.subWhereLogic);
        }
    }

    // SET-Operationen (UNION / INTERSECT / EXCEPT)
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

    // JOIN
    if (cmd.isJoin) {
        auto result = engine.executeJoins(
            cmd.tableName, cmd.joinClauses,
            cmd.whereConds, cmd.whereLogic);
        if (!cmd.orderByCols.empty()) result.sortByMulti(cmd.orderByCols);
        if (!cmd.selectColumns.empty())
            result = result.project(cmd.selectColumns);
        return result;
    }

    // GROUP BY
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

    // Einfaches SELECT (mit oder ohne WHERE)
    milansql::Table result;
    if (!cmd.whereConds.empty()) {
        auto qr = engine.selectWhere(cmd.tableName, cmd.whereConds, cmd.whereLogic);
        result = std::move(qr.table);
    } else {
        result = engine.selectAll(cmd.tableName).clone();
    }

    if (cmd.hasCaseItems && !cmd.selectItems.empty()) {
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

static void printBanner() {
    std::cout << "\n"
              << "  ╔══════════════════════════════════════════╗\n"
              << "  ║         === MilanSQL v0.1 ===            ║\n"
              << "  ║   Built with <3 by Mirwais Haidari       ║\n"
              << "  ║  Type 'help' for commands, 'exit' to quit║\n"
              << "  ╚══════════════════════════════════════════╝\n"
              << "\n";
}

int main() {
    printBanner();

    milansql::Engine             engine;
    milansql::Parser             parser;
    milansql::MilanBinaryStorage storage;
    std::string                  eingabe;

    // WAL-Cleanup beim Start (Crash-Recovery: unvollständige Transaktion verwerfen)
    std::remove("database.milan.wal");

    try {
        std::size_t tableCount = storage.loadWithCount(engine);
        if (tableCount == 0) {
            std::cout << "  Neue Datenbank gestartet.\n\n";
        } else {
            std::size_t rowCount = 0;
            for (const auto& t : engine.getAllTableNames())
                rowCount += engine.selectAll(t).rowCount();
            std::cout << "  Binary format v" << milansql::MilanBinaryStorage::FORMAT_VERSION
                      << " geladen — " << tableCount << " Tabelle(n), "
                      << rowCount << " Zeile(n) total.\n\n";
        }
    } catch (const std::exception& ex) {
        std::cout << "  WARNUNG: Laden fehlgeschlagen: " << ex.what()
                  << "\n  Starte mit leerer Datenbank.\n\n";
    }

    auto persist = [&]() {
        if (engine.isInTransaction()) return;  // Während Transaktion nicht persistieren
        try { storage.save(engine); }
        catch (const std::exception& ex) {
            std::cout << "  WARNUNG: Speichern fehlgeschlagen: " << ex.what() << "\n";
        }
    };

    while (true) {
        std::cout << "milansql> " << std::flush;
        if (!std::getline(std::cin, eingabe)) {
            std::cout << "\nAuf Wiedersehen!\n"; break;
        }
        if (eingabe.empty()) continue;

        milansql::ParsedCommand cmd = parser.parse(eingabe);

        // Subqueries auflösen: inList für IN/NOT IN-Bedingungen befüllen
        for (const auto& sq : cmd.subqueries) {
            if (sq.condIdx < cmd.whereConds.size()) {
                cmd.whereConds[sq.condIdx].inList =
                    engine.subqueryValues(
                        sq.subTable, sq.subCol,
                        sq.subWhere, sq.subWhereLogic);
            }
        }

        try {
            switch (cmd.type) {

            case milansql::CommandType::EXIT:
                std::cout << "Auf Wiedersehen!\n";
                return 0;

            case milansql::CommandType::HELP:
                printHelp();
                break;

            // ── SHOW TABLES ─────────────────────────────────────
            case milansql::CommandType::SHOW_TABLES: {
                auto namen = engine.getAllTableNames();
                auto views = engine.getAllViewNames();
                if (namen.empty() && views.empty()) {
                    std::cout << "  (Keine Tabellen oder Views vorhanden)\n\n";
                    break;
                }
                // Tabellarische Ausgabe: Name | Typ | Spalten | Zeilen
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
                    std::cout << " │";
                };
                std::cout << "\n";
                stHline("┌", "┬", "┐", "─");
                std::cout << "│";
                for (size_t i = 0; i < 4; ++i) stCell(hdr[i], w[i]);
                std::cout << "\n";
                stHline("├", "┼", "┤", "─");
                for (const auto& r : infos) {
                    std::cout << "│";
                    stCell(r.name, w[0]);
                    stCell(r.typ,  w[1]);
                    stCell(r.cols, w[2]);
                    stCell(r.rows, w[3]);
                    std::cout << "\n";
                }
                stHline("└", "┴", "┘", "─");
                std::cout << "  " << infos.size() << " Eintrag/Eintraege\n\n";
                break;
            }

            // ── SHOW INDEXES ─────────────────────────────────────
            case milansql::CommandType::SHOW_INDEXES: {
                if (cmd.tableName.empty()) {
                    std::cout << "  Fehler: SHOW INDEXES FROM tabellenname\n"; break;
                }
                std::cout << "\n";
                printIndexes(engine.getIndexes(cmd.tableName), cmd.tableName);
                break;
            }

            // ── DESCRIBE ─────────────────────────────────────────
            case milansql::CommandType::DESCRIBE: {
                if (cmd.tableName.empty()) {
                    std::cout << "  Fehler: DESCRIBE tabellenname\n"; break;
                }
                if (engine.viewExists(cmd.tableName)) {
                    std::cout << "\n  View: " << cmd.tableName << "\n"
                              << "  SQL:  " << engine.getViewSql(cmd.tableName) << "\n\n";
                } else {
                    printDescribe(engine.selectAll(cmd.tableName));
                }
                break;
            }

            // ── CREATE TABLE ─────────────────────────────────────
            case milansql::CommandType::CREATE_TABLE: {
                if (cmd.tableName.empty() || cmd.columns.empty()) {
                    std::cout << "  Fehler: CREATE TABLE name (col TYP, ...)\n"; break;
                }
                engine.createTable(cmd.tableName, cmd.columns, cmd.foreignKeys);
                persist();
                std::cout << "  Tabelle '" << cmd.tableName << "' erstellt ("
                          << cmd.columns.size() << " Spalten";
                if (!cmd.foreignKeys.empty())
                    std::cout << ", " << cmd.foreignKeys.size() << " FK";
                std::cout << ").\n\n";
                break;
            }

            // ── INSERT INTO ──────────────────────────────────────
            case milansql::CommandType::INSERT: {
                if (cmd.tableName.empty()) {
                    std::cout << "  Fehler: Kein Tabellenname.\n"; break;
                }

                // Phase 28: INSERT INTO name SELECT ...
                if (cmd.isInsertSelect) {
                    if (cmd.insertSelectSql.empty()) {
                        std::cout << "  Fehler: INSERT INTO name SELECT ...\n"; break;
                    }
                    milansql::ParsedCommand sc = parser.parse(cmd.insertSelectSql);
                    // Subqueries im SELECT-Teil auflösen
                    for (const auto& sq : sc.subqueries) {
                        if (sq.condIdx < sc.whereConds.size()) {
                            sc.whereConds[sq.condIdx].inList =
                                engine.subqueryValues(sq.subTable, sq.subCol,
                                                      sq.subWhere, sq.subWhereLogic);
                        }
                    }
                    // SELECT ausführen
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
                    // Jede Zeile einfügen
                    size_t count = 0;
                    for (const auto& row : result.rows()) {
                        engine.insertRow(cmd.tableName, row.values);
                        ++count;
                    }
                    persist();
                    std::cout << "  " << count << " Zeile(n) eingefuegt in '"
                              << cmd.tableName << "' (INSERT INTO SELECT).\n\n";
                    break;
                }

                // Phase 27/39: Multi-row VALUES (normal + UPSERT)
                const auto& rows = cmd.multiValues.empty()
                    ? std::vector<std::vector<std::string>>{cmd.values}
                    : cmd.multiValues;

                if (cmd.upsertMode == "REPLACE") {
                    size_t replaced = 0, inserted = 0;
                    for (const auto& vals : rows)
                        engine.insertOrReplace(cmd.tableName, vals) ? ++replaced : ++inserted;
                    persist();
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
                    persist();
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
                    persist();
                    if (rows.size() == 1)
                        std::cout << "  1 Zeile eingefuegt in '" << cmd.tableName << "'.\n\n";
                    else
                        std::cout << "  " << rows.size() << " Zeilen eingefuegt in '"
                                  << cmd.tableName << "'.\n\n";
                }
                break;
            }

            // ── SELECT ───────────────────────────────────────────
            case milansql::CommandType::SELECT: {
                if (cmd.tableName.empty()) {
                    std::cout << "  Fehler: Kein Tabellenname.\n"; break;
                }

                // ── Phase 41: WITH / CTE ──────────────────────────
                if (!cmd.cteList.empty()) {
                    try {
                        // Jeden CTE auswerten und als temporäre Tabelle registrieren
                        for (auto& [cteName, cteInnerSql] : cmd.cteList) {
                            milansql::ParsedCommand cteParsed = parser.parse(cteInnerSql);
                            milansql::Table cteResult =
                                executeSelectToTable(engine, parser, cteParsed);
                            engine.registerTempTable(cteName, std::move(cteResult));
                        }
                        // Hauptabfrage wie ein normales SELECT ausführen
                        // (temp-Tabellen sind jetzt in tables_ sichtbar)
                        milansql::Table mainResult =
                            executeSelectToTable(engine, parser, cmd);
                        engine.cleanupTempTables();
                        std::cout << "\n";
                        if (!cmd.orderByCols.empty())
                            mainResult.sortByMulti(cmd.orderByCols);
                        printTable(mainResult, cmd.limit, cmd.limitOffset);
                    } catch (...) {
                        engine.cleanupTempTables();
                        throw;
                    }
                    break;
                }

                // ── Phase 36: EXPLAIN ─────────────────────────────
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
                    printExplain(engine.buildExplain(req));
                    break;
                }

                // ── Phase 30: Mengenoperationen ──────────────────
                if (cmd.isSetOp) {
                    if (cmd.rightSql.empty()) {
                        std::cout << "  Fehler: " << cmd.setOp << " braucht rechte SELECT-Seite.\n\n";
                        break;
                    }
                    // Linke Seite — aus cmd bereits geparst
                    milansql::Table leftResult;
                    if (!cmd.whereConds.empty()) {
                        auto qr = engine.selectWhere(cmd.tableName,
                                                     cmd.whereConds, cmd.whereLogic);
                        leftResult = std::move(qr.table);
                    } else {
                        leftResult = engine.selectAll(cmd.tableName).clone();
                    }
                    if (!cmd.selectColumns.empty())
                        leftResult = leftResult.project(cmd.selectColumns);

                    // Rechte Seite — rightSql neu parsen + ausführen
                    milansql::ParsedCommand rc = parser.parse(cmd.rightSql);
                    for (const auto& sq : rc.subqueries) {
                        if (sq.condIdx < rc.whereConds.size())
                            rc.whereConds[sq.condIdx].inList =
                                engine.subqueryValues(sq.subTable, sq.subCol,
                                                      sq.subWhere, sq.subWhereLogic);
                    }
                    milansql::Table rightResult;
                    if (!rc.whereConds.empty()) {
                        auto qr = engine.selectWhere(rc.tableName,
                                                     rc.whereConds, rc.whereLogic);
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
                    printTable(result, cmd.limit, cmd.limitOffset);
                    break;
                }

                // ── Phase 24: View-Auflösung ─────────────────────
                if (engine.viewExists(cmd.tableName)) {
                    milansql::Table result = materializeView(engine, parser, cmd.tableName, cmd);
                    std::cout << "\n";
                    if (!cmd.orderByCols.empty())
                        result.sortByMulti(cmd.orderByCols);
                    if (!cmd.selectColumns.empty())
                        result = result.project(cmd.selectColumns);
                    if (cmd.isDistinct)
                        result.makeDistinct();
                    printTable(result, cmd.limit, cmd.limitOffset);
                    break;
                }

                // ── JOIN (INNER / LEFT / mehrere) ────────────────
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
                    printTable(result, cmd.limit, cmd.limitOffset);
                    break;
                }

                // ── GROUP BY ─────────────────────────────────────
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
                    printTable(result, cmd.limit, cmd.limitOffset);
                    break;
                }

                // ── Aggregatfunktion (MIN/MAX/AVG/SUM) ──────────
                if (cmd.isAggregate) {
                    std::string val = engine.computeAggregate(
                        cmd.tableName, cmd.aggFunc, cmd.aggCol,
                        cmd.whereConds, cmd.whereLogic);
                    std::cout << "\n  " << cmd.aggFunc << "(" << cmd.aggCol << ") = "
                              << val << " (Tabelle '" << cmd.tableName << "')";
                    if (!cmd.whereConds.empty())
                        std::cout << "  [WHERE " << whereDesc(cmd) << "]";
                    std::cout << "\n\n";
                    break;
                }

                // ── COUNT(*) [WHERE] ─────────────────────────────
                if (cmd.isCount) {
                    std::size_t n = engine.countWhere(
                        cmd.tableName, cmd.whereConds, cmd.whereLogic);
                    std::cout << "\n  COUNT(*) = " << n
                              << " (Tabelle '" << cmd.tableName << "')";
                    if (!cmd.whereConds.empty())
                        std::cout << "  [WHERE " << whereDesc(cmd) << "]";
                    std::cout << "\n\n";
                    break;
                }

                std::cout << "\n";

                // Ergebnistabelle holen
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
                              << whereDesc(cmd) << ")\n\n";
                    break;
                }

                // Phase 31/32: CASE/Func-Projektion ZUERST (Aliase für ORDER BY)
                if (cmd.hasCaseItems && !cmd.selectItems.empty()) {
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

                printTable(result, cmd.limit, cmd.limitOffset);

                if (!cmd.whereConds.empty()) {
                    std::string lbl = usedIndex ? "[INDEX SCAN]" : "[FULL SCAN] ";
                    std::cout << "  " << lbl << " " << totalFound
                              << " Zeile(n) (WHERE " << whereDesc(cmd) << ")\n\n";
                }
                break;
            }

            // ── CREATE VIEW ──────────────────────────────────────
            case milansql::CommandType::CREATE_VIEW: {
                if (cmd.tableName.empty() || cmd.viewSql.empty()) {
                    std::cout << "  Fehler: CREATE VIEW name AS SELECT ...\n"; break;
                }
                engine.createView(cmd.tableName, cmd.viewSql);
                persist();
                std::cout << "  View '" << cmd.tableName << "' erstellt.\n\n";
                break;
            }

            // ── DROP VIEW ────────────────────────────────────────
            case milansql::CommandType::DROP_VIEW: {
                if (cmd.tableName.empty()) {
                    std::cout << "  Fehler: DROP VIEW viewname\n"; break;
                }
                engine.dropView(cmd.tableName);
                persist();
                std::cout << "  View '" << cmd.tableName << "' geloescht.\n\n";
                break;
            }

            // ── CREATE INDEX ─────────────────────────────────────
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

            // ── DROP INDEX ───────────────────────────────────────
            case milansql::CommandType::DROP_INDEX: {
                if (cmd.tableName.empty() || cmd.indexName.empty()) {
                    std::cout << "  Fehler: DROP INDEX name ON tabelle\n"; break;
                }
                engine.dropIndex(cmd.tableName, cmd.indexName);
                std::cout << "  Index '" << cmd.indexName
                          << "' von '" << cmd.tableName << "' geloescht.\n\n";
                break;
            }

            // ── UPDATE SET [WHERE] ────────────────────────────────
            case milansql::CommandType::UPDATE: {
                if (cmd.tableName.empty() || cmd.updateCols.empty()) {
                    std::cout << "  Fehler: UPDATE tabelle SET col=val [, col=val ...] [WHERE ...]\n"; break;
                }
                // SET-Zusammenfassung für Ausgabe
                std::string setDesc;
                for (size_t k = 0; k < cmd.updateCols.size(); ++k) {
                    if (k > 0) setDesc += ", ";
                    setDesc += cmd.updateCols[k] + "=" + cmd.updateVals[k];
                }
                if (cmd.whereColumn.empty()) {
                    std::size_t total = engine.countRows(cmd.tableName);
                    std::cout << "  WARNUNG: Kein WHERE — alle " << total
                              << " Zeile(n) werden geaendert.\n"
                              << "  Fortfahren? (j/n): " << std::flush;
                    std::string antwort;
                    std::getline(std::cin, antwort);
                    if (antwort != "j" && antwort != "J") {
                        std::cout << "  Abgebrochen.\n\n"; break;
                    }
                    std::size_t n = engine.updateAll(
                        cmd.tableName, cmd.updateCols, cmd.updateVals);
                    persist();
                    std::cout << "  " << n << " Zeile(n) aktualisiert"
                              << " (SET " << setDesc << ")\n\n";
                } else {
                    std::size_t n = engine.updateWhere(
                        cmd.tableName,
                        cmd.updateCols, cmd.updateVals,
                        cmd.whereColumn, cmd.whereValue);
                    if (n > 0) persist();
                    std::cout << "  " << n << " Zeile(n) aktualisiert"
                              << " (SET " << setDesc
                              << " WHERE " << cmd.whereColumn
                              << " = " << cmd.whereValue << ")\n\n";
                }
                break;
            }

            // ── TRUNCATE TABLE ────────────────────────────────────
            case milansql::CommandType::TRUNCATE: {
                if (cmd.tableName.empty()) {
                    std::cout << "  Fehler: TRUNCATE TABLE tabellenname\n"; break;
                }
                engine.truncateTable(cmd.tableName);
                persist();
                std::cout << "  Tabelle '" << cmd.tableName
                          << "' geleert (Schema + Constraints behalten,"
                          << " AUTO_INCREMENT = 1).\n\n";
                break;
            }

            // ── DELETE FROM [WHERE] ───────────────────────────────
            case milansql::CommandType::DELETE: {
                if (cmd.tableName.empty()) {
                    std::cout << "  Fehler: DELETE FROM tabelle [WHERE ...]\n"; break;
                }
                if (cmd.whereColumn.empty()) {
                    std::size_t total = engine.countRows(cmd.tableName);
                    std::cout << "  WARNUNG: Kein WHERE — alle " << total
                              << " Zeile(n) werden geloescht.\n"
                              << "  Fortfahren? (j/n): " << std::flush;
                    std::string antwort;
                    std::getline(std::cin, antwort);
                    if (antwort != "j" && antwort != "J") {
                        std::cout << "  Abgebrochen.\n\n"; break;
                    }
                    std::size_t n = engine.deleteAll(cmd.tableName);
                    persist();
                    std::cout << "  " << n << " Zeile(n) geloescht.\n\n";
                } else {
                    std::size_t n = engine.deleteWhere(
                        cmd.tableName, cmd.whereColumn, cmd.whereValue);
                    if (n > 0) persist();
                    std::cout << "  " << n << " Zeile(n) geloescht"
                              << " (WHERE " << cmd.whereColumn
                              << " = " << cmd.whereValue << ")\n\n";
                }
                break;
            }

            // ── STATUS ───────────────────────────────────────────
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
                // Dateigröße ermitteln
                std::string fileSize = "?";
                {
                    std::ifstream fs("database.milan", std::ios::binary | std::ios::ate);
                    if (fs) fileSize = std::to_string(fs.tellg()) + " Bytes";
                    else    fileSize = "(noch nicht gespeichert)";
                }
                // Zeilen aufbauen, dann Breite messen
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
                size_t boxW = maxKey + maxVal + 7; // "  key<pad> : val<pad>  "
                std::string hbar(boxW, '-');
                std::string topLine  = "  ┌" + std::string(boxW, ' ') + "┐";
                std::string midLine  = "  ├" + std::string(boxW, ' ') + "┤";
                std::string botLine  = "  └" + std::string(boxW, ' ') + "┘";
                // Ersetze Leerzeichen durch ─
                for (size_t i = 3; i < topLine.size() - 3; i += 3) topLine[i] = '-'; // Trick
                // Baue korrekte Box
                auto hline = [&](const std::string& l, const std::string& r) {
                    std::cout << "  " << l;
                    for (size_t i = 0; i < boxW; ++i) std::cout << "─";
                    std::cout << r << "\n";
                };
                std::string title = "MilanSQL — Datenbank-Status";
                size_t pad = (boxW > title.size() + 2) ? (boxW - title.size()) / 2 : 1;
                std::cout << "\n";
                hline("┌", "┐");
                std::cout << "  │";
                for (size_t i = 0; i < pad; ++i) std::cout << " ";
                std::cout << title;
                size_t after = boxW - pad - title.size();
                for (size_t i = 0; i < after; ++i) std::cout << " ";
                std::cout << "│\n";
                hline("├", "┤");
                for (const auto& kv : kvs) {
                    std::cout << "  │  " << kv.key;
                    for (size_t i = kv.key.size(); i < maxKey; ++i) std::cout << " ";
                    std::cout << " : " << kv.val;
                    for (size_t i = kv.val.size(); i < maxVal; ++i) std::cout << " ";
                    std::cout << "  │\n";
                }
                hline("└", "┘");
                std::cout << "\n";
                break;
            }

            // ── SHOW CREATE TABLE ────────────────────────────────
            case milansql::CommandType::SHOW_CREATE_TABLE: {
                if (cmd.tableName.empty()) {
                    std::cout << "  Fehler: SHOW CREATE TABLE tabellenname\n"; break;
                }
                if (engine.viewExists(cmd.tableName)) {
                    std::cout << "\n  CREATE VIEW " << cmd.tableName
                              << " AS " << engine.getViewSql(cmd.tableName) << "\n\n";
                } else {
                    const auto& tbl = engine.selectAll(cmd.tableName);
                    std::cout << "\n  " << buildCreateTableSql(tbl) << "\n\n";
                }
                break;
            }

            // ── DROP TABLE ───────────────────────────────────────
            case milansql::CommandType::DROP_TABLE: {
                if (cmd.tableName.empty()) {
                    std::cout << "  Fehler: Kein Tabellenname.\n"; break;
                }
                engine.dropTable(cmd.tableName);
                persist();
                std::cout << "  Tabelle '" << cmd.tableName << "' geloescht.\n\n";
                break;
            }

            // ── ALTER TABLE ───────────────────────────────────────
            case milansql::CommandType::ALTER_TABLE: {
                if (cmd.tableName.empty() || cmd.alterOp.empty()) {
                    std::cout << "  Fehler: ALTER TABLE name ADD|DROP|RENAME COLUMN ...\n";
                    break;
                }
                engine.alterTable(cmd.tableName, cmd.alterOp,
                                  cmd.alterColName, cmd.alterColType,
                                  cmd.alterColNew);
                persist();
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

            // ── BEGIN ─────────────────────────────────────────────
            case milansql::CommandType::BEGIN:
                engine.beginTransaction("database.milan.wal");
                std::cout << "  Transaktion gestartet.\n\n";
                break;

            // ── COMMIT ────────────────────────────────────────────
            case milansql::CommandType::COMMIT:
                engine.applyAndCommit();
                persist();
                std::cout << "  Transaktion erfolgreich abgeschlossen (COMMIT).\n\n";
                break;

            // ── ROLLBACK ──────────────────────────────────────────
            case milansql::CommandType::ROLLBACK:
                engine.rollbackTransaction();
                std::cout << "  Transaktion abgebrochen (ROLLBACK).\n\n";
                break;

            case milansql::CommandType::UNKNOWN:
            default:
                std::cout << "  Unbekannter Befehl: '" << eingabe
                          << "'\n  Tippe 'help' fuer eine Uebersicht.\n\n";
                break;
            }

        } catch (const std::exception& ex) {
            std::cout << "  FEHLER: " << ex.what() << "\n\n";
        }
    }

    return 0;
}
