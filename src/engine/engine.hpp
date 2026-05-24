#pragma once

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <algorithm>
#include <stdexcept>
#include <fstream>
#include <cstdio>
#include <cstdint>

#include "btree.hpp"

// ============================================================
// engine.hpp — MilanSQL Engine (Phase 24)
// Neu: Views (CREATE VIEW / SELECT * FROM view)
// ============================================================

namespace milansql {

// ── Basis-Datenstrukturen ─────────────────────────────────────

// Phase 23: CHECK Constraint (pro Spalte)
struct CheckConstraint {
    std::string op;   // =, !=, <, >, <=, >=
    std::string val;  // rechte Seite (Literal)
};

struct Column {
    std::string name;
    std::string type;
    bool        notNull        = false;  // NOT NULL constraint
    bool        isUnique       = false;  // UNIQUE constraint
    bool        hasDefault     = false;  // ob DEFAULT gesetzt
    std::string defaultValue   = "";     // DEFAULT-Wert
    bool        isPrimaryKey   = false;  // PRIMARY KEY (impliziert NOT NULL + UNIQUE)
    bool        autoIncrement  = false;  // AUTO_INCREMENT
    std::vector<CheckConstraint> checks; // Phase 23: CHECK constraints
    Column(std::string n, std::string t)
        : name(std::move(n)), type(std::move(t)) {}
};

// ── Phase 20/21: FOREIGN KEY Definition ──────────────────────
struct ForeignKeyDef {
    std::string fromCol;              // lokale Spalte (Kind-Tabelle)
    std::string refTable;             // referenzierte Tabelle (Eltern)
    std::string refCol;               // referenzierte Spalte  (Eltern)
    std::string onDelete = "RESTRICT"; // Phase 21: CASCADE, SET NULL, RESTRICT
};

struct Row {
    std::vector<std::string> values;
    explicit Row(std::vector<std::string> v) : values(std::move(v)) {}
};

struct IndexInfo {
    std::string indexName;
    std::string colName;
    std::string type;
};

// ── EXISTS-Subquery-Spec (Phase 15) ──────────────────────────
// Speichert die korrelierte Bedingung einer EXISTS-Subquery.
struct ExistsSpec {
    std::string subTable;   // Tabelle der Subquery
    std::string condLeft;   // linke Seite der WHERE-Bedingung
    std::string condOp;     // Operator (=, !=, ...)
    std::string condRight;  // rechte Seite (Outer-Referenz oder Literal)
};

// ── WHERE-Bedingung (Phase 9 / 13 / 14 / 15) ─────────────────
struct WhereCondition {
    std::string              col;
    std::string              op;           // =, !=, <, >, <=, >=, LIKE,
                                           // IS NULL, IS NOT NULL,
                                           // IN, NOT IN,
                                           // BETWEEN, NOT BETWEEN,
                                           // EXISTS, NOT EXISTS
    std::string              val;          // für einfache Vergleiche
    std::vector<std::string> inList;       // für IN / NOT IN
    std::string              betweenLow;   // für BETWEEN / NOT BETWEEN
    std::string              betweenHigh;
    ExistsSpec               existsSpec;   // für EXISTS / NOT EXISTS
};

// ── SELECT-Listen-Eintrag (Phase 10) ─────────────────────────
struct SelectItem {
    bool        isAgg   = false;
    std::string aggFunc;   // COUNT, MIN, MAX, AVG, SUM
    std::string aggCol;    // * oder Spaltenname
    std::string colName;   // für normale Spalten
};

// ── HAVING-Bedingung (Phase 10) ───────────────────────────────
struct HavingCondition {
    std::string aggFunc;   // COUNT, MIN, MAX, AVG, SUM
    std::string aggCol;    // * oder Spaltenname
    std::string op;        // =, !=, <, >, <=, >=
    std::string val;
};

// ── JOIN-Klausel (Phase 12) ───────────────────────────────────
struct JoinClause {
    std::string joinType;  // "INNER" oder "LEFT"
    std::string table;     // Name der rechten Tabelle
    std::string onLeft;    // "tabelle.spalte" linke ON-Seite
    std::string onRight;   // "tabelle.spalte" rechte ON-Seite
};

// ------------------------------------------------------------
// Table
// ------------------------------------------------------------
class Table {
public:
    struct IndexEntry {
        std::string            name;
        std::unique_ptr<BTree> tree;
    };

    Table() = default;
    Table(std::string name, std::vector<Column> cols)
        : name_(std::move(name)), columns_(std::move(cols)) {}

    Table(Table&&)            = default;
    Table& operator=(Table&&) = default;
    Table(const Table&)       = delete;
    Table& operator=(const Table&) = delete;

    // ── DML ───────────────────────────────────────────────────

    void insert(Row row) {
        if (row.values.size() != columns_.size())
            throw std::invalid_argument(
                "Zeilenbreite (" + std::to_string(row.values.size()) +
                ") != Schema (" + std::to_string(columns_.size()) + ")");

        // ── Constraint-Prüfung ────────────────────────────────
        for (size_t i = 0; i < columns_.size() && i < row.values.size(); ++i) {
            // NOT NULL
            if (columns_[i].notNull && row.values[i] == "NULL")
                throw std::runtime_error(
                    "NOT NULL verletzt: Spalte '" + columns_[i].name +
                    "' darf nicht NULL sein.");
            // UNIQUE (NULL-Werte sind erlaubt — mehrere NULLs OK)
            if (columns_[i].isUnique && row.values[i] != "NULL") {
                for (const auto& r : rows_)
                    if (i < r.values.size() && r.values[i] == row.values[i])
                        throw std::runtime_error(
                            "UNIQUE verletzt: '" + row.values[i] +
                            "' in Spalte '" + columns_[i].name +
                            "' existiert bereits.");
            }
        }

        rows_.push_back(std::move(row));
        size_t ni = rows_.size() - 1;
        for (auto& [col, entry] : indices_) {
            int ci = colOf(col);
            if (ci >= 0 && static_cast<size_t>(ci) < rows_[ni].values.size())
                entry.tree->insert(rows_[ni].values[ci], ni);
        }
    }

    std::size_t updateWhere(std::size_t setCI, const std::string& newVal,
                            std::size_t wCI,  const std::string& wVal) {
        std::size_t n = 0;
        for (auto& row : rows_)
            if (wCI < row.values.size() && row.values[wCI] == wVal)
                { row.values[setCI] = newVal; ++n; }
        if (n) rebuildAll();
        return n;
    }

    // Phase 22: Multi-Column UPDATE
    std::size_t updateWhere(const std::vector<std::size_t>& setCIs,
                            const std::vector<std::string>& newVals,
                            std::size_t wCI, const std::string& wVal) {
        std::size_t n = 0;
        for (auto& row : rows_)
            if (wCI < row.values.size() && row.values[wCI] == wVal) {
                for (size_t k = 0; k < setCIs.size() && k < newVals.size(); ++k)
                    if (setCIs[k] < row.values.size())
                        row.values[setCIs[k]] = newVals[k];
                ++n;
            }
        if (n) rebuildAll();
        return n;
    }

    std::size_t updateAll(std::size_t setCI, const std::string& newVal) {
        for (auto& row : rows_)
            if (setCI < row.values.size()) row.values[setCI] = newVal;
        rebuildAll();
        return rows_.size();
    }

    // Phase 22: Multi-Column UPDATE ALL
    std::size_t updateAll(const std::vector<std::size_t>& setCIs,
                          const std::vector<std::string>& newVals) {
        for (auto& row : rows_)
            for (size_t k = 0; k < setCIs.size() && k < newVals.size(); ++k)
                if (setCIs[k] < row.values.size())
                    row.values[setCIs[k]] = newVals[k];
        rebuildAll();
        return rows_.size();
    }

    std::size_t deleteWhere(std::size_t wCI, const std::string& wVal) {
        std::size_t before = rows_.size();
        rows_.erase(std::remove_if(rows_.begin(), rows_.end(), [&](const Row& r) {
            return wCI < r.values.size() && r.values[wCI] == wVal;
        }), rows_.end());
        std::size_t n = before - rows_.size();
        if (n) rebuildAll();
        return n;
    }

    std::size_t deleteAll() {
        std::size_t n = rows_.size();
        rows_.clear();
        rebuildAll();
        return n;
    }

    // ── Phase 16: ALTER TABLE ─────────────────────────────────

    // ADD COLUMN: Schema erweitern, bestehende Zeilen mit DEFAULT oder NULL füllen
    void addColumn(Column col) {
        std::string fill = col.hasDefault ? col.defaultValue : "NULL";
        columns_.push_back(std::move(col));
        for (auto& row : rows_)
            row.values.push_back(fill);
    }

    // DROP COLUMN: Spalte + Werte aus allen Zeilen entfernen
    void dropColumn(const std::string& colName) {
        int ci = colOf(colName);
        if (ci < 0)
            throw std::runtime_error("ALTER TABLE: Spalte '" + colName + "' nicht gefunden.");
        columns_.erase(columns_.begin() + ci);
        for (auto& row : rows_)
            if (static_cast<size_t>(ci) < row.values.size())
                row.values.erase(row.values.begin() + ci);
        indices_.erase(colName);
        rebuildAll();
    }

    // RENAME COLUMN: Spaltenname im Schema ändern (Index-Schlüssel migrieren)
    void renameColumn(const std::string& oldName, const std::string& newName) {
        int ci = colOf(oldName);
        if (ci < 0)
            throw std::runtime_error("ALTER TABLE: Spalte '" + oldName + "' nicht gefunden.");
        if (colOf(newName) >= 0)
            throw std::runtime_error("ALTER TABLE: Spalte '" + newName + "' existiert bereits.");
        columns_[ci].name = newName;
        // Index-Eintrag umbenennen falls vorhanden
        auto it = indices_.find(oldName);
        if (it != indices_.end()) {
            indices_[newName] = std::move(it->second);
            indices_.erase(it);
        }
    }

    // ── Index-Operationen ─────────────────────────────────────

    void createIndex(const std::string& colName, const std::string& idxName) {
        int ci = colOf(colName);
        if (ci < 0)
            throw std::runtime_error("Spalte '" + colName + "' nicht gefunden.");
        auto tree = std::make_unique<BTree>();
        for (size_t ri = 0; ri < rows_.size(); ++ri)
            if (static_cast<size_t>(ci) < rows_[ri].values.size())
                tree->insert(rows_[ri].values[ci], ri);
        indices_[colName] = IndexEntry{idxName, std::move(tree)};
    }

    void dropIndex(const std::string& idxName) {
        for (auto it = indices_.begin(); it != indices_.end(); ++it)
            if (it->second.name == idxName) { indices_.erase(it); return; }
        throw std::runtime_error("Index '" + idxName + "' nicht gefunden.");
    }

    bool hasIndex(const std::string& colName) const {
        return indices_.count(colName) > 0;
    }

    std::vector<size_t> indexSearch(const std::string& colName,
                                    const std::string& val) const {
        auto it = indices_.find(colName);
        return it != indices_.end() ? it->second.tree->search(val)
                                    : std::vector<size_t>{};
    }

    std::vector<IndexInfo> getIndexes() const {
        std::vector<IndexInfo> result;
        for (const auto& [col, entry] : indices_)
            result.push_back({entry.name, col, "BTREE"});
        return result;
    }

    // ── Phase 20: FOREIGN KEY ─────────────────────────────────
    void addForeignKey(const ForeignKeyDef& fk) {
        foreignKeys_.push_back(fk);
    }
    const std::vector<ForeignKeyDef>& getForeignKeys() const {
        return foreignKeys_;
    }

    // ── Phase 19: AUTO_INCREMENT Counter ─────────────────────

    // Gibt den nächsten Wert zurück und erhöht den Zähler.
    uint64_t consumeAutoInc(const std::string& colName) {
        auto& v = autoIncMap_[colName];
        if (v == 0) v = 1;
        return v++;
    }

    // Setzt den Zähler auf minNext, wenn minNext größer ist.
    // Wird bei explizit angegebenen Werten aufgerufen.
    void updateAutoIncBase(const std::string& colName, uint64_t minNext) {
        auto& v = autoIncMap_[colName];
        if (minNext > v) v = minNext;
    }

    // Aktuellen Zählerstand abfragen (für Speicherung / DESCRIBE).
    uint64_t peekAutoInc(const std::string& colName) const {
        auto it = autoIncMap_.find(colName);
        return (it != autoIncMap_.end() && it->second > 0) ? it->second : 1;
    }

    // Zähler direkt setzen (beim Laden aus Datei).
    void setAutoInc(const std::string& colName, uint64_t val) {
        autoIncMap_[colName] = val;
    }

    // ── Phase 8: Abfrage-Hilfsmethoden ───────────────────────

    void sortBy(const std::string& col, bool desc) {
        int ci = colOf(col);
        if (ci < 0)
            throw std::runtime_error("ORDER BY: Spalte '" + col + "' nicht gefunden.");
        std::sort(rows_.begin(), rows_.end(),
            [ci, desc](const Row& a, const Row& b) {
                const std::string& va =
                    static_cast<size_t>(ci) < a.values.size() ? a.values[ci] : "";
                const std::string& vb =
                    static_cast<size_t>(ci) < b.values.size() ? b.values[ci] : "";
                try {
                    size_t ea = 0, eb = 0;
                    double da = std::stod(va, &ea);
                    double db = std::stod(vb, &eb);
                    if (ea == va.size() && eb == vb.size())
                        return desc ? da > db : da < db;
                } catch (...) {}
                return desc ? va > vb : va < vb;
            });
    }

    void makeDistinct() {
        std::vector<Row> uniq;
        for (auto& row : rows_) {
            bool found = false;
            for (const auto& u : uniq)
                if (u.values == row.values) { found = true; break; }
            if (!found) uniq.push_back(std::move(row));
        }
        rows_ = std::move(uniq);
    }

    Table project(const std::vector<std::string>& colNames) const {
        if (colNames.empty()) return clone();
        std::vector<int> cis;
        std::vector<Column> newCols;
        for (const auto& cname : colNames) {
            int ci = colOf(cname);
            if (ci < 0)
                throw std::runtime_error(
                    "SELECT: Spalte '" + cname + "' nicht gefunden.");
            cis.push_back(ci);
            newCols.push_back(columns_[static_cast<size_t>(ci)]);
        }
        Table result(name_, std::move(newCols));
        for (const auto& row : rows_) {
            std::vector<std::string> vals;
            vals.reserve(cis.size());
            for (int ci : cis)
                vals.push_back(
                    static_cast<size_t>(ci) < row.values.size()
                    ? row.values[ci] : "");
            result.rows_.push_back(Row(std::move(vals)));
        }
        return result;
    }

    Table clone() const {
        Table t(name_, columns_);
        t.rows_        = rows_;
        t.autoIncMap_  = autoIncMap_;
        t.foreignKeys_ = foreignKeys_;
        return t;
    }

    // ── Getter ────────────────────────────────────────────────
    const std::vector<Row>&    rows()     const { return rows_;    }
    const std::vector<Column>& columns()  const { return columns_; }
    const std::string&         name()     const { return name_;    }
    std::size_t                rowCount() const { return rows_.size(); }

    // Öffentlich für Engine::alterTable()
    int colOf(const std::string& col) const {
        for (size_t i = 0; i < columns_.size(); ++i)
            if (columns_[i].name == col) return static_cast<int>(i);
        return -1;
    }

private:
    std::string         name_;
    std::vector<Column> columns_;
    std::vector<Row>    rows_;
    std::map<std::string, IndexEntry>  indices_;
    std::map<std::string, uint64_t>    autoIncMap_;      // colName → nächster Wert
    std::vector<ForeignKeyDef>         foreignKeys_;     // FK-Definitionen

    void rebuildAll() {
        for (auto& [colName, entry] : indices_) {
            int ci = colOf(colName);
            if (ci < 0) continue;
            entry.tree->clear();
            for (size_t ri = 0; ri < rows_.size(); ++ri)
                if (static_cast<size_t>(ci) < rows_[ri].values.size())
                    entry.tree->insert(rows_[ri].values[ci], ri);
        }
    }
};

// ── Gepufferte Transaktion-Operation (Phase 17) ───────────────
struct BufferedOp {
    enum class Type {
        INSERT,
        UPDATE_WHERE, UPDATE_ALL,
        DELETE_WHERE, DELETE_ALL,
        ALTER
    };

    Type        opType;
    std::string tableName;

    // INSERT
    std::vector<std::string> values;

    // UPDATE_WHERE / UPDATE_ALL
    std::string setCol, setVal;

    // UPDATE_WHERE / DELETE_WHERE (einfache Einzelbedingung)
    std::string whereCol, whereVal;

    // ALTER
    std::string alterOp, alterColName, alterColType, alterColNew;
};

// ------------------------------------------------------------
// Engine
// ------------------------------------------------------------
class Engine {
public:
    Engine() = default;

    // ── DDL ───────────────────────────────────────────────────

    void createTable(const std::string& name, std::vector<Column> cols,
                     std::vector<ForeignKeyDef> fks = {}) {
        if (tables_.count(name))
            throw std::runtime_error("Tabelle '" + name + "' existiert bereits.");
        tables_.emplace(name, Table(name, std::move(cols)));
        for (const auto& fk : fks)
            tables_.at(name).addForeignKey(fk);
    }

    void createIndex(const std::string& tbl, const std::string& col,
                     const std::string& idxName) {
        getTable(tbl).createIndex(col, idxName);
    }

    void dropIndex(const std::string& tbl, const std::string& idxName) {
        getTable(tbl).dropIndex(idxName);
    }

    void dropTable(const std::string& name) {
        if (!tables_.erase(name))
            throw std::runtime_error("Tabelle '" + name + "' nicht gefunden.");
    }

    // AUTO_INCREMENT-Zähler setzen (wird beim Laden aus Datei aufgerufen)
    void setTableAutoInc(const std::string& tbl,
                         const std::string& col, uint64_t val) {
        getTable(tbl).setAutoInc(col, val);
    }

    // FOREIGN KEY hinzufügen (wird beim Laden aus Datei aufgerufen)
    void addForeignKey(const std::string& tbl, const ForeignKeyDef& fk) {
        getTable(tbl).addForeignKey(fk);
    }

    // ── DML ───────────────────────────────────────────────────

    void insertRow(const std::string& tbl, std::vector<std::string> vals) {
        Table& t = getTable(tbl);       // wirft wenn Tabelle fehlt
        applyDefaults(t, vals);         // fehlende Werte mit DEFAULT/NULL füllen
        applyAutoInc(t, vals);          // AUTO_INCREMENT-Spalten befüllen
        checkInsertFK(tbl, vals);       // FOREIGN KEY prüfen
        checkAllConstraints(tbl, vals); // Phase 23: CHECK constraints prüfen

        if (inTransaction_) {
            if (vals.size() != t.columns().size())
                throw std::invalid_argument(
                    "Zeilenbreite (" + std::to_string(vals.size()) +
                    ") != Schema (" + std::to_string(t.columns().size()) + ")");
            BufferedOp op;
            op.opType    = BufferedOp::Type::INSERT;
            op.tableName = tbl;
            op.values    = std::move(vals);
            appendWal(op);
            txBuffer_.push_back(std::move(op));
            return;
        }
        t.insert(Row(std::move(vals)));
    }

    struct WhereResult {
        Table table;
        bool  usedIndex;
    };

    const Table& selectAll(const std::string& tbl) const {
        return getTable(tbl);
    }

    // SELECT mit WHERE (AND / OR, alle Operatoren inkl. LIKE)
    WhereResult selectWhere(const std::string& tblName,
                            const std::vector<WhereCondition>& conds,
                            const std::string& logic = "AND") const {
        const Table& src = getTable(tblName);
        Table result(tblName, src.columns());
        bool usedIndex = false;

        if (conds.empty()) {
            for (const auto& row : src.rows()) result.insert(row);
            return {std::move(result), false};
        }

        // Index nur bei einzelner "="-Bedingung
        if (conds.size() == 1 && conds[0].op == "=" && src.hasIndex(conds[0].col)) {
            usedIndex = true;
            for (size_t ri : src.indexSearch(conds[0].col, conds[0].val))
                if (ri < src.rows().size()) result.insert(src.rows()[ri]);
            return {std::move(result), usedIndex};
        }

        // EXISTS/NOT EXISTS benötigen rowMatches (mit Engine-Kontext)
        bool hasExists = false;
        for (const auto& cond : conds)
            if (cond.op == "EXISTS" || cond.op == "NOT EXISTS")
                { hasExists = true; break; }

        if (hasExists) {
            for (const auto& row : src.rows())
                if (rowMatches(src, row, conds, logic)) result.insert(row);
            return {std::move(result), false};
        }

        std::vector<std::size_t> cis;
        cis.reserve(conds.size());
        for (const auto& cond : conds)
            cis.push_back(colIdx(src, cond.col));

        for (const auto& row : src.rows()) {
            if (rowMatchesByIdx(row, conds, cis, logic))
                result.insert(row);
        }
        return {std::move(result), usedIndex};
    }

    // COUNT(*) [mit optionalem WHERE]
    std::size_t countRows(const std::string& tbl) const {
        return getTable(tbl).rowCount();
    }

    std::size_t countWhere(const std::string& tblName,
                           const std::vector<WhereCondition>& conds,
                           const std::string& logic = "AND") const {
        if (conds.empty()) return countRows(tblName);
        return selectWhere(tblName, conds, logic).table.rowCount();
    }

    // Aggregatfunktion ohne GROUP BY
    std::string computeAggregate(const std::string& tblName,
                                  const std::string& func,
                                  const std::string& col,
                                  const std::vector<WhereCondition>& conds,
                                  const std::string& logic = "AND") const {
        const Table& src = getTable(tblName);
        std::size_t ci = colIdx(src, col);
        std::vector<double> nums;
        for (const auto& row : src.rows()) {
            if (!rowMatches(src, row, conds, logic)) continue;
            if (ci < row.values.size()) {
                try { nums.push_back(std::stod(row.values[ci])); }
                catch (...) {}
            }
        }
        if (nums.empty()) return "NULL";
        if (func == "MIN") return formatNum(*std::min_element(nums.begin(), nums.end()));
        if (func == "MAX") return formatNum(*std::max_element(nums.begin(), nums.end()));
        if (func == "SUM") {
            double s = 0; for (double v : nums) s += v;
            return formatNum(s);
        }
        if (func == "AVG") {
            double s = 0; for (double v : nums) s += v;
            return formatNum(s / static_cast<double>(nums.size()));
        }
        return "NULL";
    }

    // ── Phase 10: GROUP BY ────────────────────────────────────

    // Führt eine GROUP BY-Abfrage aus.
    // Reihenfolge: WHERE → GROUP → SELECT/AGG → HAVING
    // ORDER BY und LIMIT werden danach in main.cpp angewendet.
    Table groupBy(const std::string& tblName,
                  const std::vector<WhereCondition>& whereConds,
                  const std::string& whereLogic,
                  const std::vector<std::string>& groupCols,
                  const std::vector<SelectItem>& selectItems,
                  const std::vector<HavingCondition>& havingConds,
                  const std::string& havingLogic) const {

        const Table& src = getTable(tblName);

        if (groupCols.empty())
            throw std::runtime_error("GROUP BY: keine Spalten angegeben.");
        if (selectItems.empty())
            throw std::runtime_error("GROUP BY: leere SELECT-Liste.");

        // Step 1 — WHERE-Filter: indizes der passenden Zeilen sammeln
        std::vector<size_t> filtered;
        filtered.reserve(src.rows().size());
        for (size_t i = 0; i < src.rows().size(); ++i)
            if (rowMatches(src, src.rows()[i], whereConds, whereLogic))
                filtered.push_back(i);

        // Step 2 — GROUP BY-Spaltenindizes vorab auflösen
        std::vector<size_t> groupCIs;
        groupCIs.reserve(groupCols.size());
        for (const auto& gc : groupCols)
            groupCIs.push_back(colIdx(src, gc));

        // Step 3 — Zeilen gruppieren (Reihenfolge des ersten Auftretens erhalten)
        std::map<std::string, std::vector<size_t>> groupMap;
        std::vector<std::string> groupOrder;

        for (size_t ri : filtered) {
            const Row& row = src.rows()[ri];
            // Schlüssel: Werte der GROUP BY-Spalten, durch \x01 getrennt
            std::string key;
            for (size_t ci : groupCIs) {
                key += '\x01';
                key += (ci < row.values.size() ? row.values[ci] : "");
            }
            if (!groupMap.count(key)) groupOrder.push_back(key);
            groupMap[key].push_back(ri);
        }

        // Step 4 — Ergebnis-Spalten aus SelectItems ableiten
        std::vector<Column> resultCols;
        resultCols.reserve(selectItems.size());
        for (const auto& item : selectItems) {
            std::string name = item.isAgg
                ? (item.aggFunc + "(" + item.aggCol + ")")
                : item.colName;
            resultCols.emplace_back(name, "");
        }
        Table result("", std::move(resultCols));

        // Step 5 — Pro Gruppe: Werte berechnen, HAVING prüfen, Zeile einfügen
        for (const auto& key : groupOrder) {
            const auto& riList = groupMap[key];

            std::vector<std::string> vals;
            vals.reserve(selectItems.size());

            for (const auto& item : selectItems) {
                if (item.isAgg) {
                    vals.push_back(computeAggForGroup(src, item, riList));
                } else {
                    size_t ci = colIdx(src, item.colName);
                    const Row& first = src.rows()[riList[0]];
                    vals.push_back(ci < first.values.size() ? first.values[ci] : "");
                }
            }

            if (satisfiesHaving(havingConds, havingLogic, selectItems, vals))
                result.insert(Row(vals));
        }

        return result;
    }

    // ── Phase 11: INNER JOIN ──────────────────────────────────

    // Nested-Loop INNER JOIN zweier Tabellen.
    // Ergebnis-Spalten heißen "tabellenname.spaltenname".
    // onT1Col / onT2Col: einfacher Spaltenname (ohne Tabellen-Prefix).
    Table innerJoin(const std::string& t1Name, const std::string& t2Name,
                    const std::string& onT1Col, const std::string& onT2Col,
                    const std::vector<WhereCondition>& whereConds,
                    const std::string& whereLogic) const {
        const Table& t1 = getTable(t1Name);
        const Table& t2 = getTable(t2Name);

        // Ergebnis-Schema: "t1.col", "t2.col"
        std::vector<Column> resultCols;
        resultCols.reserve(t1.columns().size() + t2.columns().size());
        for (const auto& c : t1.columns())
            resultCols.emplace_back(t1Name + "." + c.name, c.type);
        for (const auto& c : t2.columns())
            resultCols.emplace_back(t2Name + "." + c.name, c.type);

        Table result("", std::move(resultCols));

        size_t ci1 = colIdx(t1, onT1Col);
        size_t ci2 = colIdx(t2, onT2Col);

        // Nested Loop
        for (const auto& row1 : t1.rows()) {
            const std::string& k1 =
                ci1 < row1.values.size() ? row1.values[ci1] : "";
            for (const auto& row2 : t2.rows()) {
                const std::string& k2 =
                    ci2 < row2.values.size() ? row2.values[ci2] : "";
                if (k1 != k2) continue;

                // Zeilen zusammenführen
                std::vector<std::string> vals;
                vals.reserve(row1.values.size() + row2.values.size());
                vals.insert(vals.end(), row1.values.begin(), row1.values.end());
                vals.insert(vals.end(), row2.values.begin(), row2.values.end());

                Row combined(vals);
                if (rowMatches(result, combined, whereConds, whereLogic))
                    result.insert(std::move(combined));
            }
        }
        return result;
    }

    // ── Phase 12: Multi-JOIN (INNER + LEFT) ──────────────────

    // Führt eine Kette von JOINs aus (INNER oder LEFT).
    // Ergebnis-Spalten: "tabelle.spalte" (qualifiziert).
    // WHERE wird auf das Gesamtergebnis angewendet.
    Table executeJoins(const std::string& baseName,
                       const std::vector<JoinClause>& joins,
                       const std::vector<WhereCondition>& whereConds,
                       const std::string& whereLogic) const {

        const Table& base = getTable(baseName);

        // Startergebnis: Spalten qualifizieren
        std::vector<Column> initCols;
        initCols.reserve(base.columns().size());
        for (const auto& c : base.columns())
            initCols.emplace_back(baseName + "." + c.name, c.type);

        Table current("", initCols);
        for (const auto& r : base.rows()) current.insert(r);

        // Jeden JOIN-Schritt nacheinander anwenden
        for (const auto& jc : joins) {
            const Table& right = getTable(jc.table);

            // Neues Schema: bisherige Spalten + neue qualifizierte Spalten
            std::vector<Column> newCols = current.columns();
            for (const auto& c : right.columns())
                newCols.emplace_back(jc.table + "." + c.name, c.type);

            Table next("", newCols);
            const size_t rightWidth = right.columns().size();

            // ON-Seiten zuordnen: eine Seite gehört zur rechten Tabelle, die andere
            // zum bisherigen Ergebnis. Seiten ggf. tauschen.
            auto tblOf = [](const std::string& s) -> std::string {
                auto p = s.find('.');
                return p != std::string::npos ? s.substr(0, p) : "";
            };
            std::string resultSide = jc.onLeft;
            std::string rightSide  = jc.onRight;
            if (tblOf(jc.onLeft) == jc.table)
                std::swap(resultSide, rightSide);

            size_t leftCI  = findQualColIdx(current, resultSide);
            size_t rightCI = findRawColIdx (right,   rightSide);

            for (const auto& lrow : current.rows()) {
                const std::string& lval =
                    leftCI < lrow.values.size() ? lrow.values[leftCI] : "";
                bool matched = false;

                for (const auto& rrow : right.rows()) {
                    const std::string& rval =
                        rightCI < rrow.values.size() ? rrow.values[rightCI] : "";
                    if (lval != rval) continue;

                    std::vector<std::string> vals;
                    vals.reserve(lrow.values.size() + rrow.values.size());
                    vals.insert(vals.end(), lrow.values.begin(), lrow.values.end());
                    vals.insert(vals.end(), rrow.values.begin(), rrow.values.end());
                    next.insert(Row(vals));
                    matched = true;
                }

                // LEFT JOIN: nicht gematchte Zeilen mit NULL auffüllen
                if (!matched && jc.joinType == "LEFT") {
                    std::vector<std::string> vals;
                    vals.reserve(lrow.values.size() + rightWidth);
                    vals.insert(vals.end(), lrow.values.begin(), lrow.values.end());
                    for (size_t k = 0; k < rightWidth; ++k) vals.push_back("NULL");
                    next.insert(Row(vals));
                }
            }
            current = std::move(next);
        }

        // WHERE auf Gesamtergebnis anwenden
        if (whereConds.empty()) return current;

        Table filtered("", current.columns());
        for (const auto& row : current.rows())
            if (rowMatches(current, row, whereConds, whereLogic))
                filtered.insert(row);
        return filtered;
    }

    // ── Phase 16: ALTER TABLE ────────────────────────────────

    // op: "ADD", "DROP", "RENAME"
    void alterTable(const std::string& tblName,
                    const std::string& op,
                    const std::string& colName,
                    const std::string& colType,    // nur für ADD
                    const std::string& newName) {  // nur für RENAME
        if (inTransaction_) {
            BufferedOp bufOp;
            bufOp.opType       = BufferedOp::Type::ALTER;
            bufOp.tableName    = tblName;
            bufOp.alterOp      = op;
            bufOp.alterColName = colName;
            bufOp.alterColType = colType;
            bufOp.alterColNew  = newName;
            appendWal(bufOp);
            txBuffer_.push_back(std::move(bufOp));
            return;
        }
        Table& t = getTable(tblName);
        if (op == "ADD") {
            if (t.colOf(colName) >= 0)
                throw std::runtime_error(
                    "ALTER TABLE: Spalte '" + colName + "' existiert bereits.");
            t.addColumn(Column(colName, colType.empty() ? "TEXT" : colType));
        } else if (op == "DROP") {
            t.dropColumn(colName);
        } else if (op == "RENAME") {
            t.renameColumn(colName, newName);
        } else {
            throw std::runtime_error("ALTER TABLE: unbekannte Operation '" + op + "'.");
        }
    }

    // ── Phase 14: Subquery-Hilfsmethode ──────────────────────
    // Führt "SELECT col FROM tbl [WHERE ...]" aus und gibt
    // alle Werte der ersten Ergebnisspalte als Liste zurück.
    std::vector<std::string> subqueryValues(
            const std::string& tblName,
            const std::string& col,
            const std::vector<WhereCondition>& conds,
            const std::string& logic) const {
        const Table& src = getTable(tblName);
        size_t ci = colIdx(src, col);
        std::vector<std::string> result;
        for (const auto& row : src.rows()) {
            if (!rowMatches(src, row, conds, logic)) continue;
            if (ci < row.values.size())
                result.push_back(row.values[ci]);
        }
        return result;
    }

    // ── Mutation ──────────────────────────────────────────────

    std::size_t updateWhere(const std::string& tbl,
                            const std::string& setCol, const std::string& setVal,
                            const std::string& wCol,   const std::string& wVal) {
        checkSetConstraints(tbl, {setCol}, {setVal});  // Phase 23
        if (inTransaction_) {
            BufferedOp op;
            op.opType    = BufferedOp::Type::UPDATE_WHERE;
            op.tableName = tbl;
            op.setCol    = setCol;  op.setVal  = setVal;
            op.whereCol  = wCol;    op.whereVal = wVal;
            appendWal(op);
            txBuffer_.push_back(std::move(op));
            return 0;
        }
        Table& t = getTable(tbl);
        return t.updateWhere(colIdx(t, setCol), setVal, colIdx(t, wCol), wVal);
    }

    std::size_t updateAll(const std::string& tbl,
                          const std::string& setCol, const std::string& setVal) {
        checkSetConstraints(tbl, {setCol}, {setVal});  // Phase 23
        if (inTransaction_) {
            BufferedOp op;
            op.opType    = BufferedOp::Type::UPDATE_ALL;
            op.tableName = tbl;
            op.setCol    = setCol;  op.setVal = setVal;
            appendWal(op);
            txBuffer_.push_back(std::move(op));
            return 0;
        }
        Table& t = getTable(tbl);
        return t.updateAll(colIdx(t, setCol), setVal);
    }

    std::size_t deleteWhere(const std::string& tbl,
                            const std::string& wCol, const std::string& wVal) {
        // Phase 21: CASCADE / SET NULL / RESTRICT für betroffene Zeilen
        {
            const Table& t = getTable(tbl);
            size_t wCI = colIdx(t, wCol);
            std::vector<Row> matched;
            for (const auto& row : t.rows())
                if (wCI < row.values.size() && row.values[wCI] == wVal)
                    matched.push_back(row);
            for (const auto& row : matched)
                cascadeDelete(tbl, row);
        }
        if (inTransaction_) {
            BufferedOp op;
            op.opType    = BufferedOp::Type::DELETE_WHERE;
            op.tableName = tbl;
            op.whereCol  = wCol;  op.whereVal = wVal;
            appendWal(op);
            txBuffer_.push_back(std::move(op));
            return 0;
        }
        Table& t = getTable(tbl);
        return t.deleteWhere(colIdx(t, wCol), wVal);
    }

    // Phase 22: Multi-Column UPDATE WHERE
    std::size_t updateWhere(const std::string& tbl,
                            const std::vector<std::string>& setCols,
                            const std::vector<std::string>& setVals,
                            const std::string& wCol, const std::string& wVal) {
        checkSetConstraints(tbl, setCols, setVals);  // Phase 23
        if (inTransaction_) {
            // In Transaktion: als mehrere Einzel-Ops puffern
            for (size_t k = 0; k < setCols.size() && k < setVals.size(); ++k)
                updateWhere(tbl, setCols[k], setVals[k], wCol, wVal);
            return 0;
        }
        Table& t = getTable(tbl);
        std::vector<std::size_t> setCIs;
        for (const auto& col : setCols)
            setCIs.push_back(colIdx(t, col));
        return t.updateWhere(setCIs, setVals, colIdx(t, wCol), wVal);
    }

    // Phase 22: Multi-Column UPDATE ALL
    std::size_t updateAll(const std::string& tbl,
                          const std::vector<std::string>& setCols,
                          const std::vector<std::string>& setVals) {
        checkSetConstraints(tbl, setCols, setVals);  // Phase 23
        if (inTransaction_) {
            for (size_t k = 0; k < setCols.size() && k < setVals.size(); ++k)
                updateAll(tbl, setCols[k], setVals[k]);
            return 0;
        }
        Table& t = getTable(tbl);
        std::vector<std::size_t> setCIs;
        for (const auto& col : setCols)
            setCIs.push_back(colIdx(t, col));
        return t.updateAll(setCIs, setVals);
    }

    std::size_t deleteAll(const std::string& tbl) {
        // Phase 21: CASCADE / SET NULL / RESTRICT für alle Zeilen
        {
            std::vector<Row> rowsCopy(getTable(tbl).rows().begin(),
                                      getTable(tbl).rows().end());
            for (const auto& row : rowsCopy)
                cascadeDelete(tbl, row);
        }
        if (inTransaction_) {
            BufferedOp op;
            op.opType    = BufferedOp::Type::DELETE_ALL;
            op.tableName = tbl;
            appendWal(op);
            txBuffer_.push_back(std::move(op));
            return 0;
        }
        return getTable(tbl).deleteAll();
    }

    // ── Phase 22: TRUNCATE TABLE ─────────────────────────────
    // Löscht alle Zeilen, behält Schema + Constraints.
    // AUTO_INCREMENT Counter wird auf 1 zurückgesetzt.
    // Keine FK-Prüfung (wie MySQL TRUNCATE — DDL-Semantik).
    void truncateTable(const std::string& tbl) {
        Table& t = getTable(tbl);
        t.deleteAll();   // direkte Table-Methode, ohne FK-Check
        for (const auto& col : t.columns())
            if (col.autoIncrement)
                t.setAutoInc(col.name, 1);
    }

    // ── Phase 17: Transaktionen ──────────────────────────────

    bool isInTransaction() const { return inTransaction_; }

    // BEGIN: WAL-Datei anlegen, Puffer leeren, Transaktion starten
    void beginTransaction(const std::string& walPath) {
        if (inTransaction_)
            throw std::runtime_error("Transaktion bereits aktiv.");
        walPath_ = walPath;
        std::remove(walPath_.c_str());   // alte WAL löschen (Crash-Recovery)
        inTransaction_ = true;
        txBuffer_.clear();
    }

    // COMMIT: alle gepufferten Ops auf Tabellen anwenden,
    //         Puffer leeren, Transaktion beenden, WAL löschen.
    //         Persistierung (save) muss der Aufrufer danach selbst machen.
    void applyAndCommit() {
        if (!inTransaction_)
            throw std::runtime_error("Keine aktive Transaktion.");
        for (const auto& op : txBuffer_)
            applyOp(op);
        txBuffer_.clear();
        inTransaction_ = false;
        deleteWal();
    }

    // ROLLBACK: Puffer verwerfen, Transaktion abbrechen, WAL löschen.
    //           Die Tabellen bleiben unverändert.
    void rollbackTransaction() {
        if (!inTransaction_)
            throw std::runtime_error("Keine aktive Transaktion.");
        txBuffer_.clear();
        inTransaction_ = false;
        deleteWal();
    }

    // WAL-Datei löschen (wird auch von main.cpp beim Start aufgerufen)
    void deleteWal() {
        if (!walPath_.empty())
            std::remove(walPath_.c_str());
    }

    // ── Metadaten ─────────────────────────────────────────────

    std::vector<IndexInfo> getIndexes(const std::string& tbl) const {
        return getTable(tbl).getIndexes();
    }

    std::vector<std::string> getAllTableNames() const {
        std::vector<std::string> names;
        for (const auto& [n, _] : tables_) names.push_back(n);
        return names;
    }

    bool tableExists(const std::string& n) const { return tables_.count(n) > 0; }

    // ── Phase 24: Views ───────────────────────────────────────

    void createView(const std::string& name, const std::string& sql) {
        if (views_.count(name))
            throw std::runtime_error("View '" + name + "' existiert bereits.");
        if (tables_.count(name))
            throw std::runtime_error("'" + name + "' ist bereits eine Tabelle.");
        views_[name] = sql;
    }

    void dropView(const std::string& name) {
        if (!views_.erase(name))
            throw std::runtime_error("View '" + name + "' nicht gefunden.");
    }

    bool viewExists(const std::string& n) const { return views_.count(n) > 0; }

    const std::string& getViewSql(const std::string& n) const {
        auto it = views_.find(n);
        if (it == views_.end())
            throw std::runtime_error("View '" + n + "' nicht gefunden.");
        return it->second;
    }

    std::vector<std::string> getAllViewNames() const {
        std::vector<std::string> names;
        for (const auto& [n, _] : views_) names.push_back(n);
        return names;
    }

    // Filtert eine beliebige Table mit WHERE-Bedingungen.
    // Wird für Views mit äußerem WHERE-Filter verwendet.
    Table filterTable(const Table& src,
                      const std::vector<WhereCondition>& conds,
                      const std::string& logic) const {
        if (conds.empty()) return src.clone();
        Table result("", src.columns());
        for (const auto& row : src.rows())
            if (rowMatches(src, row, conds, logic))
                result.insert(row);
        return result;
    }

private:
    // ── Tabellen-Zugriff ──────────────────────────────────────

    Table& getTable(const std::string& n) {
        auto it = tables_.find(n);
        if (it == tables_.end())
            throw std::runtime_error("Tabelle '" + n + "' nicht gefunden.");
        return it->second;
    }
    const Table& getTable(const std::string& n) const {
        auto it = tables_.find(n);
        if (it == tables_.end())
            throw std::runtime_error("Tabelle '" + n + "' nicht gefunden.");
        return it->second;
    }

    // Spaltenindex; wirft bei unbekannter Spalte
    static std::size_t colIdx(const Table& t, const std::string& col) {
        for (size_t i = 0; i < t.columns().size(); ++i)
            if (t.columns()[i].name == col) return i;
        throw std::runtime_error(
            "Spalte '" + col + "' nicht gefunden in '" + t.name() + "'.");
    }

    // Spalte im Ergebnis suchen (qualifizierte Namen "t.col").
    // Exakter Match, dann Suffix-Match.
    static size_t findQualColIdx(const Table& t, const std::string& qual) {
        for (size_t i = 0; i < t.columns().size(); ++i)
            if (t.columns()[i].name == qual) return i;
        auto dot = qual.find('.');
        std::string raw = dot != std::string::npos ? qual.substr(dot + 1) : qual;
        for (size_t i = 0; i < t.columns().size(); ++i) {
            const auto& cn = t.columns()[i].name;
            auto p = cn.find('.');
            std::string suf = p != std::string::npos ? cn.substr(p + 1) : cn;
            if (suf == raw) return i;
        }
        throw std::runtime_error(
            "JOIN ON: Spalte '" + qual + "' nicht im Ergebnis gefunden.");
    }

    // Spalte in roher Tabelle suchen (unqualifizierte Namen).
    // Entfernt Tabellen-Prefix falls vorhanden.
    static size_t findRawColIdx(const Table& t, const std::string& qual) {
        auto dot = qual.find('.');
        const std::string raw = dot != std::string::npos ? qual.substr(dot + 1) : qual;
        for (size_t i = 0; i < t.columns().size(); ++i)
            if (t.columns()[i].name == raw) return i;
        throw std::runtime_error(
            "JOIN ON: Spalte '" + qual + "' in rechter Tabelle nicht gefunden.");
    }

    // ── WHERE-Auswertung ──────────────────────────────────────

    // Einzelbedingung auswerten (IN / NOT IN / BETWEEN / NOT BETWEEN)
    static bool evalCond(const std::string& val, const WhereCondition& c) {
        if (c.op == "IN" || c.op == "NOT IN") {
            bool found = false;
            for (const auto& v : c.inList)
                if (val == v) { found = true; break; }
            return c.op == "IN" ? found : !found;
        }
        if (c.op == "BETWEEN" || c.op == "NOT BETWEEN") {
            bool inRange = false;
            try {
                double v  = std::stod(val);
                double lo = std::stod(c.betweenLow);
                double hi = std::stod(c.betweenHigh);
                inRange = (v >= lo && v <= hi);
            } catch (...) {
                inRange = (val >= c.betweenLow && val <= c.betweenHigh);
            }
            return c.op == "BETWEEN" ? inRange : !inRange;
        }
        return compareValues(val, c.op, c.val);
    }

    // EXISTS-Subquery auswerten (korrelierter Vergleich mit Outer-Zeile)
    bool evalExists(const Table& outer, const Row& outerRow,
                    const WhereCondition& c) const {
        const ExistsSpec& spec = c.existsSpec;
        const Table& sub = getTable(spec.subTable);

        // Hilfsfunktion: Spalte in Tabelle suchen (mit/ohne Tabellenpräfix)
        auto findCI = [](const Table& t, const std::string& ref) -> int {
            for (size_t i = 0; i < t.columns().size(); ++i)
                if (t.columns()[i].name == ref) return (int)i;
            auto dot = ref.find('.');
            if (dot != std::string::npos) {
                std::string bare = ref.substr(dot + 1);
                for (size_t i = 0; i < t.columns().size(); ++i)
                    if (t.columns()[i].name == bare) return (int)i;
            }
            return -1;
        };

        // Bestimmen: welche Seite ist im Sub-Table, welche im Outer-Table?
        int subLeft  = findCI(sub,   spec.condLeft);
        int subRight = findCI(sub,   spec.condRight);

        std::string innerColRef, outerRef;
        if (subLeft >= 0 && subRight < 0) {
            innerColRef = spec.condLeft;
            outerRef    = spec.condRight;
        } else if (subRight >= 0 && subLeft < 0) {
            innerColRef = spec.condRight;
            outerRef    = spec.condLeft;
        } else {
            // Beide in Sub-Table oder Literal auf rechter Seite
            innerColRef = spec.condLeft;
            outerRef    = spec.condRight;
        }

        // Outer-Wert auflösen
        int outerCI = findCI(outer, outerRef);
        std::string outerVal = (outerCI >= 0 &&
                                (size_t)outerCI < outerRow.values.size())
                               ? outerRow.values[outerCI]
                               : outerRef;  // Fallback: Literal

        // Sub-Table scannen
        int innerCI = findCI(sub, innerColRef);
        if (innerCI < 0) return c.op == "NOT EXISTS";

        for (const auto& subRow : sub.rows()) {
            if ((size_t)innerCI < subRow.values.size() &&
                compareValues(subRow.values[innerCI], spec.condOp, outerVal))
                return c.op == "EXISTS";  // Match → EXISTS=true, NOT EXISTS=false
        }
        return c.op == "NOT EXISTS";  // Kein Match → EXISTS=false, NOT EXISTS=true
    }

    // Prüft ob eine Zeile alle/eine Bedingungen erfüllt (allgemeine Version)
    bool rowMatches(const Table& src,
                    const Row& row,
                    const std::vector<WhereCondition>& conds,
                    const std::string& logic) const {
        if (conds.empty()) return true;

        auto evalOne = [&](const WhereCondition& c) -> bool {
            if (c.op == "EXISTS" || c.op == "NOT EXISTS")
                return evalExists(src, row, c);
            size_t ci = colIdx(src, c.col);
            return ci < row.values.size() && evalCond(row.values[ci], c);
        };

        if (logic == "AND") {
            for (const auto& c : conds) if (!evalOne(c)) return false;
            return true;
        }
        for (const auto& c : conds) if (evalOne(c)) return true;
        return false;
    }

    // Schnellere Version mit vorberechneten Spaltenindizes
    // (kein EXISTS-Support — EXISTS braucht rowMatches mit Engine-Kontext)
    static bool rowMatchesByIdx(const Row& row,
                                 const std::vector<WhereCondition>& conds,
                                 const std::vector<std::size_t>& cis,
                                 const std::string& logic) {
        if (logic == "AND") {
            for (size_t k = 0; k < conds.size(); ++k)
                if (!(cis[k] < row.values.size() &&
                      evalCond(row.values[cis[k]], conds[k])))
                    return false;
            return true;
        }
        for (size_t k = 0; k < conds.size(); ++k)
            if (cis[k] < row.values.size() &&
                evalCond(row.values[cis[k]], conds[k]))
                return true;
        return false;
    }

    // ── GROUP BY-Hilfsmethoden ────────────────────────────────

    // Aggregat für eine Gruppe berechnen
    static std::string computeAggForGroup(const Table& src,
                                           const SelectItem& item,
                                           const std::vector<size_t>& riList) {
        if (item.aggFunc == "COUNT")
            return std::to_string(riList.size());

        size_t ci = colIdx(src, item.aggCol);
        std::vector<double> nums;
        nums.reserve(riList.size());
        for (size_t ri : riList) {
            const Row& row = src.rows()[ri];
            if (ci < row.values.size()) {
                try { nums.push_back(std::stod(row.values[ci])); }
                catch (...) {}
            }
        }
        if (nums.empty()) return "NULL";
        if (item.aggFunc == "MIN") return formatNum(*std::min_element(nums.begin(), nums.end()));
        if (item.aggFunc == "MAX") return formatNum(*std::max_element(nums.begin(), nums.end()));
        if (item.aggFunc == "SUM") {
            double s = 0; for (double v : nums) s += v;
            return formatNum(s);
        }
        if (item.aggFunc == "AVG") {
            double s = 0; for (double v : nums) s += v;
            return formatNum(s / static_cast<double>(nums.size()));
        }
        return "NULL";
    }

    // HAVING-Bedingungen gegen berechnete Gruppen-Werte prüfen
    static bool satisfiesHaving(const std::vector<HavingCondition>& conds,
                                  const std::string& logic,
                                  const std::vector<SelectItem>& items,
                                  const std::vector<std::string>& vals) {
        if (conds.empty()) return true;

        auto eval = [&](const HavingCondition& hc) {
            for (size_t k = 0; k < items.size(); ++k)
                if (items[k].isAgg &&
                    items[k].aggFunc == hc.aggFunc &&
                    items[k].aggCol  == hc.aggCol)
                    return compareValues(vals[k], hc.op, hc.val);
            return false;
        };

        if (logic == "AND") {
            for (const auto& hc : conds) if (!eval(hc)) return false;
            return true;
        }
        for (const auto& hc : conds) if (eval(hc)) return true;
        return false;
    }

    // ── Vergleichs- und Format-Hilfsfunktionen ────────────────

    // LIKE: % = beliebig viele Zeichen, _ = genau eines (DP-Ansatz)
    static bool likeMatch(const std::string& s, const std::string& p) {
        size_t m = s.size(), n = p.size();
        std::vector<std::vector<bool>> dp(m + 1, std::vector<bool>(n + 1, false));
        dp[0][0] = true;
        for (size_t j = 1; j <= n; ++j)
            if (p[j - 1] == '%') dp[0][j] = dp[0][j - 1];
        for (size_t i = 1; i <= m; ++i)
            for (size_t j = 1; j <= n; ++j) {
                if (p[j - 1] == '%')
                    dp[i][j] = dp[i - 1][j] || dp[i][j - 1];
                else if (p[j - 1] == '_' || p[j - 1] == s[i - 1])
                    dp[i][j] = dp[i - 1][j - 1];
            }
        return dp[m][n];
    }

    // Vergleich zweier Werte — numerisch wenn beide Seiten Zahlen sind
    static bool compareValues(const std::string& a,
                               const std::string& op,
                               const std::string& b) {
        if (op == "LIKE")        return likeMatch(a, b);
        if (op == "IS NULL")     return a == "NULL";
        if (op == "IS NOT NULL") return a != "NULL";
        try {
            size_t ea = 0, eb = 0;
            double da = std::stod(a, &ea);
            double db = std::stod(b, &eb);
            if (ea == a.size() && eb == b.size()) {
                if (op == "=")  return da == db;
                if (op == "!=") return da != db;
                if (op == "<")  return da <  db;
                if (op == ">")  return da >  db;
                if (op == "<=") return da <= db;
                if (op == ">=") return da >= db;
            }
        } catch (...) {}
        if (op == "=")  return a == b;
        if (op == "!=") return a != b;
        if (op == "<")  return a <  b;
        if (op == ">")  return a >  b;
        if (op == "<=") return a <= b;
        if (op == ">=") return a >= b;
        return false;
    }

    // Zahl formatieren: Integer wenn möglich, sonst Dezimal ohne Null-Anhang
    static std::string formatNum(double v) {
        long long iv = static_cast<long long>(v);
        if (static_cast<double>(iv) == v) return std::to_string(iv);
        std::string s = std::to_string(v);
        auto dot = s.find('.');
        if (dot != std::string::npos) {
            size_t last = s.find_last_not_of('0');
            if (last == dot) return s.substr(0, dot);
            return s.substr(0, last + 1);
        }
        return s;
    }

    // ── Phase 18: DEFAULT-Werte auffüllen ────────────────────

    // Füllt fehlende Werte am Ende mit DEFAULT-Wert oder "NULL" auf.
    // Wird vor jedem INSERT aufgerufen, um kürzere VALUES-Listen zu erlauben.
    static void applyDefaults(const Table& t, std::vector<std::string>& vals) {
        while (vals.size() < t.columns().size()) {
            const Column& col = t.columns()[vals.size()];
            vals.push_back(col.hasDefault ? col.defaultValue : "NULL");
        }
    }

    // ── Phase 19: AUTO_INCREMENT befüllen ────────────────────

    // Für jede AUTO_INCREMENT-Spalte:
    //   - Wert ist "NULL" / leer → nächsten Counter-Wert einsetzen
    //   - Expliziter Wert        → Counter auf max(counter, val+1) setzen
    static void applyAutoInc(Table& t, std::vector<std::string>& vals) {
        for (size_t i = 0; i < t.columns().size() && i < vals.size(); ++i) {
            if (!t.columns()[i].autoIncrement) continue;
            const std::string& colName = t.columns()[i].name;
            if (vals[i].empty() || vals[i] == "NULL") {
                vals[i] = std::to_string(t.consumeAutoInc(colName));
            } else {
                try {
                    uint64_t v = std::stoull(vals[i]);
                    t.updateAutoIncBase(colName, v + 1);
                } catch (...) {}
            }
        }
    }

    // ── Phase 23: CHECK Constraint Prüfung ───────────────────

    // INSERT: prüft alle CHECK-Bedingungen für alle Spalten.
    void checkAllConstraints(const std::string& tblName,
                             const std::vector<std::string>& vals) const {
        const Table& t = getTable(tblName);
        for (size_t i = 0; i < t.columns().size() && i < vals.size(); ++i) {
            const Column& col = t.columns()[i];
            if (col.checks.empty()) continue;
            if (vals[i] == "NULL") continue;   // NULL überspringt CHECK
            for (const auto& cc : col.checks)
                if (!compareValues(vals[i], cc.op, cc.val))
                    throw std::runtime_error(
                        "CHECK constraint verletzt: " + col.name +
                        " " + cc.op + " " + cc.val);
        }
    }

    // UPDATE: prüft CHECK-Bedingungen nur für die gesetzten Spalten.
    void checkSetConstraints(const std::string& tblName,
                              const std::vector<std::string>& setCols,
                              const std::vector<std::string>& setVals) const {
        const Table& t = getTable(tblName);
        for (size_t k = 0; k < setCols.size() && k < setVals.size(); ++k) {
            int ci = t.colOf(setCols[k]);
            if (ci < 0) continue;
            const Column& col = t.columns()[ci];
            if (col.checks.empty()) continue;
            if (setVals[k] == "NULL") continue;
            for (const auto& cc : col.checks)
                if (!compareValues(setVals[k], cc.op, cc.val))
                    throw std::runtime_error(
                        "CHECK constraint verletzt: " + col.name +
                        " " + cc.op + " " + cc.val);
        }
    }

    // ── Phase 20: FOREIGN KEY Checks ─────────────────────────

    // INSERT: prüft ob alle FK-Werte in der Eltern-Tabelle existieren.
    void checkInsertFK(const std::string& tblName,
                       const std::vector<std::string>& vals) const {
        const Table& child = getTable(tblName);
        for (const auto& fk : child.getForeignKeys()) {
            int fromCI = child.colOf(fk.fromCol);
            if (fromCI < 0 || static_cast<size_t>(fromCI) >= vals.size()) continue;
            const std::string& val = vals[fromCI];
            if (val == "NULL") continue;  // NULL → kein Parent nötig

            // Eltern-Tabelle suchen
            auto it = tables_.find(fk.refTable);
            if (it == tables_.end())
                throw std::runtime_error(
                    "FOREIGN KEY: Elterntabelle '" + fk.refTable + "' nicht gefunden.");
            const Table& parent = it->second;
            int refCI = parent.colOf(fk.refCol);
            if (refCI < 0)
                throw std::runtime_error(
                    "FOREIGN KEY: Spalte '" + fk.refCol +
                    "' in '" + fk.refTable + "' nicht gefunden.");
            bool found = false;
            for (const auto& pRow : parent.rows())
                if (static_cast<size_t>(refCI) < pRow.values.size() &&
                    pRow.values[refCI] == val)
                    { found = true; break; }
            if (!found)
                throw std::runtime_error(
                    "FOREIGN KEY verletzt: Wert '" + val + "' existiert nicht in " +
                    fk.refTable + "(" + fk.refCol + ").");
        }
    }

    // Phase 21: ON DELETE CASCADE / SET NULL / RESTRICT
    // Wendet die FK-Aktion auf alle Kind-Zeilen an, die auf parentRow verweisen.
    void cascadeDelete(const std::string& parentName, const Row& parentRow) {
        const Table& parent = getTable(parentName);
        for (auto& [childName, childTbl] : tables_) {
            if (childName == parentName) continue;
            for (const auto& fk : childTbl.getForeignKeys()) {
                if (fk.refTable != parentName) continue;
                int refCI = parent.colOf(fk.refCol);
                if (refCI < 0 ||
                    static_cast<size_t>(refCI) >= parentRow.values.size()) continue;
                const std::string& refVal = parentRow.values[refCI];
                if (refVal == "NULL") continue;
                int fromCI = childTbl.colOf(fk.fromCol);
                if (fromCI < 0) continue;

                if (fk.onDelete == "RESTRICT") {
                    for (const auto& cRow : childTbl.rows())
                        if (static_cast<size_t>(fromCI) < cRow.values.size() &&
                            cRow.values[fromCI] == refVal)
                            throw std::runtime_error(
                                "FOREIGN KEY verletzt (RESTRICT): '" + refVal +
                                "' wird in " + childName + "(" + fk.fromCol +
                                ") referenziert — Loeschen nicht erlaubt.");
                } else if (fk.onDelete == "CASCADE") {
                    childTbl.deleteWhere(static_cast<size_t>(fromCI), refVal);
                } else if (fk.onDelete == "SET NULL") {
                    childTbl.updateWhere(static_cast<size_t>(fromCI), "NULL",
                                         static_cast<size_t>(fromCI), refVal);
                }
            }
        }
    }

    // ── Phase 17: WAL + Transaktion (privat) ─────────────────

    // Op an WAL-Datei anhängen (append-only, Text-Format)
    void appendWal(const BufferedOp& op) {
        if (walPath_.empty()) return;
        std::ofstream wal(walPath_, std::ios::app);
        if (!wal) return;
        wal << "OP " << static_cast<int>(op.opType)
            << " " << op.tableName << "\n";
        for (const auto& v : op.values)
            wal << "VAL " << v << "\n";
        if (!op.setCol.empty())
            wal << "SET " << op.setCol << " " << op.setVal << "\n";
        if (!op.whereCol.empty())
            wal << "WHERE " << op.whereCol << " " << op.whereVal << "\n";
        if (!op.alterOp.empty())
            wal << "ALTER " << op.alterOp
                << " " << op.alterColName
                << " " << op.alterColType
                << " " << op.alterColNew << "\n";
        wal << "---\n";
    }

    // Eine gepufferte Op auf die Tabellen anwenden
    void applyOp(const BufferedOp& op) {
        switch (op.opType) {
            case BufferedOp::Type::INSERT: {
                getTable(op.tableName).insert(Row(op.values));
                break;
            }
            case BufferedOp::Type::UPDATE_WHERE: {
                Table& t = getTable(op.tableName);
                t.updateWhere(colIdx(t, op.setCol), op.setVal,
                              colIdx(t, op.whereCol), op.whereVal);
                break;
            }
            case BufferedOp::Type::UPDATE_ALL: {
                Table& t = getTable(op.tableName);
                t.updateAll(colIdx(t, op.setCol), op.setVal);
                break;
            }
            case BufferedOp::Type::DELETE_WHERE: {
                Table& t = getTable(op.tableName);
                t.deleteWhere(colIdx(t, op.whereCol), op.whereVal);
                break;
            }
            case BufferedOp::Type::DELETE_ALL: {
                getTable(op.tableName).deleteAll();
                break;
            }
            case BufferedOp::Type::ALTER: {
                // Direkt ausführen (inTransaction_ ist hier schon false
                // oder wir rufen den inneren ALTER-Zweig auf)
                Table& t = getTable(op.tableName);
                if (op.alterOp == "ADD") {
                    if (t.colOf(op.alterColName) < 0)
                        t.addColumn(Column(op.alterColName,
                                          op.alterColType.empty() ? "TEXT"
                                                                   : op.alterColType));
                } else if (op.alterOp == "DROP") {
                    t.dropColumn(op.alterColName);
                } else if (op.alterOp == "RENAME") {
                    t.renameColumn(op.alterColName, op.alterColNew);
                }
                break;
            }
        }
    }

    std::map<std::string, Table>  tables_;
    std::map<std::string, std::string> views_;   // Phase 24: name → SQL

    // Transaktionszustand
    bool                     inTransaction_ = false;
    std::string              walPath_;
    std::vector<BufferedOp>  txBuffer_;
};

} // namespace milansql
