#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>
#include <memory>
#include <algorithm>
#include <stdexcept>
#include <fstream>
#include <cstdio>
#include <cstdint>
#include <cmath>
#include <climits>
#include <numeric>
#include <sstream>
#include <iomanip>
#include <limits>
#include <functional>
#include <regex>
#include <thread>
#include <unordered_map>
#include <cstring>

#include "btree.hpp"
#include "../cache/query_cache.hpp"
#include "../utils/date_utils.hpp"
#include "../utils/json_utils.hpp"
#include "../scheduler/event_scheduler.hpp"
#include "../locking/lock_manager.hpp"
#include "../spatial/spatial.hpp"      // Phase 70: Spatial Index
#include "../mvcc/transaction_manager.hpp"  // Phase 71: MVCC
#include "../mvcc/vacuum.hpp"               // Phase 85: Auto-Vacuum
#include "../wal/checkpoint.hpp"            // Phase 85: WAL Checkpointing
#include "../buffer/buffer_pool.hpp"   // Phase 73: Buffer Pool Manager
#include "../parallel/parallel_executor.hpp" // Phase 77: Parallel Query
#include "../replication/logical_repl.hpp"  // Phase 81: Logical Replication
#include "../types/array_type.hpp"          // Phase 88: Array Data Type
#include "../types/vector_type.hpp"         // Phase 111: pgvector
#include "../fdw/foreign_data_wrapper.hpp"  // Phase 89: FDW base
#include "../concurrent/rwlock.hpp"         // Phase 112: per-table RwLocks
#include "../concurrent/atomic_table.hpp"   // Phase 112: lock-free stats
#include "../optimizer/dp_planner.hpp"      // Phase 113: DP Join Order Optimizer
#include "../optimizer/histogram.hpp"       // Phase 113: Histogram Selectivity
#include "../fdw/csv_fdw.hpp"               // Phase 89: CSV FDW
#include "../fdw/http_fdw.hpp"              // Phase 89: HTTP/JSON FDW
#include "../extensions/extension_manager.hpp" // Phase 90: Extension System
#include "../extensions/builtin/vector_ext.hpp" // Phase 111: pgvector functions
#include "../compression/compressor.hpp"       // Phase 96: Data Compression
#include "../compression/rle_compressor.hpp"
#include "../compression/lz4_compressor.hpp"
#include "../compression/dict_compressor.hpp"
#include "../compression/zstd_compressor.hpp"
#include "../timeseries/timeseries_manager.hpp" // Phase 97: Time-Series
#include "../cdc/cdc_manager.hpp"               // Phase 98: CDC
#include "../federation/federation_manager.hpp" // Phase 105: Query Federation

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
    // Phase 68: Generated Columns
    bool        isGenerated    = false;  // GENERATED ALWAYS AS
    std::string generatedExpr  = "";     // expression string
    bool        isStored       = false;  // STORED (true) or VIRTUAL (false)
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
    // Phase 71: MVCC version stamps
    uint64_t xmin = 0;  // tx that created this row (0 = always visible / auto-commit)
    uint64_t xmax = 0;  // tx that deleted this row (0 = alive)
    explicit Row(std::vector<std::string> v) : values(std::move(v)) {}
};

struct IndexInfo {
    std::string indexName;
    std::string colName;   // Phase 35: alle Spalten als ", "-getrennte Liste
    std::string type;
};

// ── Phase 36: EXPLAIN ─────────────────────────────────────────
struct ExplainStep {
    int         nr;
    std::string op;       // SCAN, INDEX, FILTER, JOIN, GROUP, AGGREGATE, SORT, LIMIT, PROJECT, SET_OP
    std::string table;    // Tabellenname oder "-"
    std::string details;  // Bedingung / Spalten / Typ
    std::string index;    // Index-Name oder "-"
};

struct ExplainPlan {
    std::vector<ExplainStep> steps;
};

// ── Phase 62: Partitioning ────────────────────────────────────
enum class PartitionType { NONE, RANGE, LIST, HASH };

struct PartitionRangeDef {
    std::string name;
    std::string limitStr;   // "100" or "MAXVALUE"
    long long   limit;      // numeric (LLONG_MAX for MAXVALUE)
};

struct PartitionListDef {
    std::string name;
    std::vector<std::string> values;
};

struct PartitionInfo {
    PartitionType type = PartitionType::NONE;
    std::string   column;
    std::vector<PartitionRangeDef> ranges;
    std::vector<PartitionListDef>  lists;
    int           hashCount = 0;
    bool hasPartitions() const { return type != PartitionType::NONE; }
};

// ── Phase 37: Korrelierte Subquery-Strukturen ─────────────────
// Eine Bedingung in der WHERE-Klausel einer Subquery.
// val kann ein Literal ODER ein korrelierter Verweis (alias.col) sein —
// zur Laufzeit wird geprüft, ob val in der äußeren Tabelle liegt.
struct SubCond {
    std::string col;  // Spalte der Sub-Tabelle (ggf. mit Alias-Prefix: b.kunden_id)
    std::string op;   // Operator (=, !=, <, >, <=, >=)
    std::string val;  // Literal oder äußerer Verweis (z.B. k.id)
};

// Spec für eine skalare Subquery: SELECT aggfunc(col) FROM tbl WHERE ...
struct ScalarSubSpec {
    std::string subTable;           // Sub-Tabelle
    std::string aggFunc;            // COUNT, AVG, SUM, MIN, MAX (oder "" für Rohwert)
    std::string aggCol;             // * oder Spaltenname
    std::vector<SubCond> conds;     // WHERE-Bedingungen
    std::string whereLogic = "AND"; // AND oder OR
};

// ── EXISTS-Subquery-Spec (Phase 15 / Phase 37 erweitert) ──────
struct ExistsSpec {
    std::string subTable;   // Tabelle der Subquery
    std::string condLeft;   // Legacy: linke Seite (Phase 15)
    std::string condOp;     // Legacy: Operator
    std::string condRight;  // Legacy: rechte Seite
    // Phase 37: Multi-Bedingung
    std::vector<SubCond> subConds;
    std::string subWhereLogic = "AND";
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
    // Phase 37: Korrelierte Scalar Subquery auf rechter Seite
    bool          isScalarSub = false;
    ScalarSubSpec scalarSub;
    // Phase 40: LHS ist eine Funktion wie CAST(...)
    bool          isFuncLhs   = false;
    std::string   funcLhsExpr;  // space-separated token string: "CAST ( col AS INT )"

    // Phase 49: MATCH(...) AGAINST('query') in WHERE
    bool                     isMatchAgainst = false;
    std::vector<std::string> matchCols;
    std::string              againstQuery;

    // Default constructor (all fields zero-/empty-initialized)
    WhereCondition() = default;
    // Convenience constructor used in parser for simple conditions
    WhereCondition(std::string c, std::string o, std::string v)
        : col(std::move(c)), op(std::move(o)), val(std::move(v)) {}
};

// ── SELECT-Listen-Eintrag (Phase 10 / Phase 31 / Phase 32) ───
struct SelectItem {
    bool        isAgg   = false;
    std::string aggFunc;   // COUNT, MIN, MAX, AVG, SUM
    std::string aggCol;    // * oder Spaltenname
    std::string colName;   // für normale Spalten
    std::string alias;     // Phase 31: AS alias

    // Phase 31: CASE WHEN THEN ELSE END
    bool isCaseExpr = false;
    struct WhenClause {
        std::string col;     // linke Seite der Bedingung
        std::string op;      // =, !=, <, >, <=, >=
        std::string val;     // rechte Seite
        std::string result;  // THEN-Wert
    };
    std::vector<WhenClause> caseWhen;
    std::string             caseElse = "NULL";

    // Phase 32: String-Funktionen (UPPER/LOWER/LENGTH/CONCAT/SUBSTR/TRIM/REPLACE)
    bool                     isFuncExpr = false;
    std::string              funcName;
    std::vector<std::string> funcArgs;   // Spaltenname ODER String-Literal
    // Phase 37: Skalare Subquery in SELECT-Liste
    bool          isScalarSubquery = false;
    ScalarSubSpec scalarSub;

    // Phase 49: MATCH(...) AGAINST('query') AS score in SELECT
    bool                     isMatchAgainst = false;
    std::vector<std::string> matchCols;
    std::string              againstQuery;

    // Phase 42: Window Functions
    bool        isWindowFunc     = false;
    std::string windowFunc;          // ROW_NUMBER, RANK, DENSE_RANK, SUM, AVG, COUNT, MIN, MAX
    std::string windowFuncArg;       // argument for SUM/AVG/MIN/MAX (column name), or "*" for COUNT
    std::string windowPartitionBy;   // column name for PARTITION BY, empty = no partition
    std::string windowOrderBy;       // column name for ORDER BY inside OVER
    bool        windowOrderDesc = false;  // true = DESC

    // Phase 88: UNNEST
    bool        isUnnest    = false;
    std::string unnestCol;           // column name to unnest
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
    std::string joinType;   // "INNER", "LEFT", "RIGHT", "FULL"
    std::string table;      // Name der rechten Tabelle
    std::string tableAlias; // optionaler Alias der rechten Tabelle (Phase 87)
    std::string onLeft;     // "tabelle.spalte" linke ON-Seite
    std::string onRight;    // "tabelle.spalte" rechte ON-Seite
};

// ── Phase 49: Full-Text Index ─────────────────────────────────
struct FullTextIndex {
    std::string name;
    std::string tableName;
    std::vector<std::string> cols;
    // inverted index: word → list of row indices
    std::map<std::string, std::vector<size_t>> invertedIndex;
    // forward index: row idx → word counts (for TF scoring)
    std::map<size_t, std::map<std::string,int>> forwardIndex;
    // total word counts per row
    std::map<size_t, int> rowWordCount;
};

// ── Phase 43: Trigger-Definition ─────────────────────────────
struct TriggerDef {
    std::string name;
    std::string timing;       // "BEFORE" or "AFTER"
    std::string event;        // "INSERT", "UPDATE", "DELETE"
    std::string tableName;
    std::string body;         // raw SQL body text (between BEGIN and END)
    std::string granularity;  // Phase 93: "ROW" (default) or "STATEMENT"
};

// ── Phase 44: Stored Procedure Definition ────────────────────
struct ProcedureDef {
    std::string name;
    std::vector<std::pair<std::string,std::string>> params; // {paramName, type}
    std::string body; // raw SQL body between BEGIN and END
};

// ── Phase 45: Prepared Statement ─────────────────────────────
struct PreparedStmt {
    std::string name;
    std::string sql;        // original SQL with ? placeholders
    int paramCount = 0;     // number of ? placeholders
};

// ── Phase 46: User Definition ─────────────────────────────────
struct UserDef {
    std::string name;
    std::size_t passwordHash = 0;  // std::hash<std::string>{}(password)
    // grants: table name → set of privileges ("SELECT","INSERT","UPDATE","DELETE","ALL")
    std::map<std::string, std::set<std::string>> grants;
};

// ── Phase 36: EXPLAIN — Übergabe-Struct (alle engine.hpp-Typen verfügbar) ──
struct ExplainRequest {
    std::string tableName;
    bool isJoin = false;
    std::vector<JoinClause> joinClauses;
    std::vector<WhereCondition> whereConds;
    std::string whereLogic = "AND";
    bool isGroupBy = false;
    std::vector<std::string> groupByCols;
    std::vector<HavingCondition> havingConds;
    bool isAggregate = false;
    std::string aggFunc, aggCol;
    std::vector<SelectItem> selectItems;
    bool hasCaseItems = false;
    std::vector<std::string> selectColumns;
    std::vector<std::pair<std::string,bool>> orderByCols;  // Phase 38
    int limit = -1;
    int limitOffset = 0;  // Phase 38
    bool isSetOp = false;
    std::string setOp;
};

// ------------------------------------------------------------
// Table
// ------------------------------------------------------------
class Table {
public:
    struct IndexEntry {
        std::string              name;  // Index-Name (= Map-Schlüssel)
        std::vector<std::string> cols;  // alle Spalten; cols[0] = führende Spalte
        std::unique_ptr<BTree>   tree;  // BTree, nach cols[0]-Wert indiziert
        std::string              type = "BTREE";  // Phase 70: "BTREE" or "SPATIAL"
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
                    if (r.xmax == 0 &&  // Phase 71: skip logically deleted rows
                        i < r.values.size() && r.values[i] == row.values[i])
                        throw std::runtime_error(
                            "UNIQUE verletzt: '" + row.values[i] +
                            "' in Spalte '" + columns_[i].name +
                            "' existiert bereits.");
            }
        }

        rows_.push_back(std::move(row));
        size_t ni = rows_.size() - 1;
        for (auto& [idxName, entry] : indices_) {
            if (entry.cols.empty()) continue;
            int ci = colOf(entry.cols[0]);
            if (ci >= 0 && static_cast<size_t>(ci) < rows_[ni].values.size()
                && rows_[ni].xmax == 0)  // Phase 71: don't index dead rows
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

    // ── Phase 39: UPSERT-Hilfsmethoden ───────────────────────

    // Gibt Indices aller Zeilen zurück, die mit vals auf UNIQUE/PK-Spalten kollidieren
    std::vector<size_t> conflictRows(const std::vector<std::string>& vals) const {
        std::vector<bool> flag(rows_.size(), false);
        for (size_t i = 0; i < columns_.size() && i < vals.size(); ++i) {
            if (!columns_[i].isUnique) continue;
            if (vals[i] == "NULL")     continue;  // NULL kollidiert nicht
            for (size_t r = 0; r < rows_.size(); ++r)
                if (rows_[r].xmax == 0 &&  // Phase 71: skip deleted rows
                    i < rows_[r].values.size() && rows_[r].values[i] == vals[i])
                    flag[r] = true;
        }
        std::vector<size_t> result;
        for (size_t r = 0; r < flag.size(); ++r)
            if (flag[r]) result.push_back(r);
        return result;
    }

    // Löscht Zeilen anhand von Indices (absteigende Reihenfolge, dann rebuildAll)
    void eraseByIndices(std::vector<size_t> idxs) {
        std::sort(idxs.begin(), idxs.end(), std::greater<size_t>());
        for (size_t idx : idxs)
            if (idx < rows_.size())
                rows_.erase(rows_.begin() + static_cast<std::ptrdiff_t>(idx));
        if (!idxs.empty()) rebuildAll();
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
        // Indizes entfernen, die die gelöschte Spalte enthalten
        for (auto it = indices_.begin(); it != indices_.end(); ) {
            const auto& c = it->second.cols;
            if (std::find(c.begin(), c.end(), colName) != c.end())
                it = indices_.erase(it);
            else ++it;
        }
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
        // Spaltennamen in allen Index-Einträgen aktualisieren
        for (auto& [idxName, entry] : indices_)
            for (auto& c : entry.cols)
                if (c == oldName) c = newName;
    }

    // ── Index-Operationen ─────────────────────────────────────

    void createIndex(const std::vector<std::string>& cols, const std::string& idxName,
                     const std::string& idxType = "BTREE") {
        if (cols.empty())
            throw std::runtime_error("CREATE INDEX: Keine Spalten angegeben.");
        int ci = colOf(cols[0]);
        if (ci < 0)
            throw std::runtime_error("Spalte '" + cols[0] + "' nicht gefunden.");
        // Prüfen ob Index-Name bereits vergeben
        if (indices_.count(idxName))
            throw std::runtime_error("Index '" + idxName + "' existiert bereits.");
        auto tree = std::make_unique<BTree>();
        for (size_t ri = 0; ri < rows_.size(); ++ri)
            if (static_cast<size_t>(ci) < rows_[ri].values.size())
                tree->insert(rows_[ri].values[ci], ri);
        IndexEntry entry;
        entry.name = idxName;
        entry.cols = cols;
        entry.tree = std::move(tree);
        entry.type = idxType;
        indices_[idxName] = std::move(entry);
    }

    void dropIndex(const std::string& idxName) {
        if (!indices_.erase(idxName))
            throw std::runtime_error("Index '" + idxName + "' nicht gefunden.");
    }

    bool hasIndex(const std::string& colName) const {
        for (const auto& [idxName, entry] : indices_)
            if (!entry.cols.empty() && entry.cols[0] == colName) return true;
        return false;
    }

    std::vector<size_t> indexSearch(const std::string& colName,
                                    const std::string& val) const {
        for (const auto& [idxName, entry] : indices_)
            if (!entry.cols.empty() && entry.cols[0] == colName)
                return entry.tree->search(val);
        return {};
    }

    std::vector<IndexInfo> getIndexes() const {
        std::vector<IndexInfo> result;
        for (const auto& [idxName, entry] : indices_) {
            // Alle Spalten als ", "-getrennte Liste anzeigen
            std::string colList;
            for (size_t i = 0; i < entry.cols.size(); ++i) {
                if (i > 0) colList += ", ";
                colList += entry.cols[i];
            }
            result.push_back({entry.name, colList, entry.type});
        }
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

    // Phase 38: Multi-Column ORDER BY
    void sortByMulti(const std::vector<std::pair<std::string,bool>>& cols) {
        if (cols.empty()) return;
        // Resolve column indices (ignore unknown columns)
        std::vector<std::pair<int,bool>> idxCols;
        for (const auto& p : cols) {
            int ci = colOf(p.first);
            if (ci >= 0) idxCols.push_back({ci, p.second});
        }
        if (idxCols.empty()) return;
        std::sort(rows_.begin(), rows_.end(),
            [&idxCols](const Row& a, const Row& b) {
                for (const auto& ic : idxCols) {
                    int ci     = ic.first;
                    bool desc  = ic.second;
                    const std::string& va =
                        static_cast<size_t>(ci) < a.values.size() ? a.values[ci] : "";
                    const std::string& vb =
                        static_cast<size_t>(ci) < b.values.size() ? b.values[ci] : "";
                    // Try numeric comparison
                    try {
                        size_t ea = 0, eb = 0;
                        double da = std::stod(va, &ea);
                        double db = std::stod(vb, &eb);
                        if (ea == va.size() && eb == vb.size()) {
                            if (da != db) return desc ? da > db : da < db;
                            continue;  // equal — try next column
                        }
                    } catch (...) {}
                    // String comparison
                    if (va != vb) return desc ? va > vb : va < vb;
                }
                return false;  // all columns equal
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
            if (row.xmax != 0) continue;  // Phase 71: skip dead rows
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
        for (const auto& row : rows_)
            if (row.xmax == 0) t.rows_.push_back(row);  // Phase 71: skip dead rows
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

    // Phase 44: Mutable row access and index rebuild for expression-based updates
    std::vector<Row>& mutableRows() { return rows_; }
    void rebuildIndexes() { rebuildAll(); }

    // ── Phase 71: MVCC helpers ────────────────────────────────

    // Logical delete: stamp xmax on matching rows (instead of physical delete)
    std::size_t stampDeleteWhere(std::size_t wCI, const std::string& wVal, uint64_t txId) {
        std::size_t n = 0;
        for (auto& row : rows_)
            if (row.xmax == 0 && wCI < row.values.size() && row.values[wCI] == wVal)
                { row.xmax = txId; ++n; }
        if (n) rebuildAll();
        return n;
    }

    // Logical delete all alive rows
    std::size_t stampDeleteAll(uint64_t txId) {
        std::size_t n = 0;
        for (auto& row : rows_)
            if (row.xmax == 0) { row.xmax = txId; ++n; }
        if (n) rebuildAll();
        return n;
    }

    // Stamp xmin on the most recently inserted row
    void stampXminLast(uint64_t txId) {
        if (!rows_.empty()) rows_.back().xmin = txId;
    }

    // VACUUM: physically remove rows whose xmax is committed (callable predicate)
    std::size_t vacuum(const std::function<bool(uint64_t)>& isCommitted) {
        std::size_t before = rows_.size();
        rows_.erase(std::remove_if(rows_.begin(), rows_.end(),
            [&](const Row& r) { return r.xmax != 0 && isCommitted(r.xmax); }),
            rows_.end());
        std::size_t n = before - rows_.size();
        if (n) rebuildAll();
        return n;
    }

    // Count dead (xmax'd) rows
    std::size_t deadRowCount() const {
        std::size_t n = 0;
        for (const auto& r : rows_) if (r.xmax != 0) ++n;
        return n;
    }

    // Phase 96: Compression metadata
    CompressionType compressionType = CompressionType::NONE;
    DictCompressor  dictCompressor;

    // Phase 62: Partition accessors
    void setPartitionInfo(const PartitionInfo& pi) { partitionInfo_ = pi; }
    const PartitionInfo& getPartitionInfo() const { return partitionInfo_; }

    // Returns the partition name for a given row (based on partition column value)
    std::string getPartitionName(const Row& row) const {
        if (!partitionInfo_.hasPartitions()) return "";
        int ci = colOf(partitionInfo_.column);
        std::string val = (ci >= 0 && static_cast<size_t>(ci) < row.values.size())
                          ? row.values[ci] : "";
        // Strip surrounding single quotes (stored as 'value' by parser)
        if (val.size() >= 2 && val.front() == '\'' && val.back() == '\'')
            val = val.substr(1, val.size() - 2);
        if (partitionInfo_.type == PartitionType::RANGE) {
            long long numVal = 0;
            try { numVal = std::stoll(val); } catch (...) { numVal = 0; }
            for (auto& r : partitionInfo_.ranges) {
                if (numVal < r.limit) return r.name;
            }
            return "";
        }
        if (partitionInfo_.type == PartitionType::LIST) {
            for (auto& l : partitionInfo_.lists) {
                for (auto& v : l.values) {
                    if (v == val) return l.name;
                }
            }
            return "";
        }
        if (partitionInfo_.type == PartitionType::HASH) {
            if (partitionInfo_.hashCount <= 0) return "";
            long long h = 0;
            try { h = std::stoll(val); } catch (...) {
                for (char c : val) h = h * 31 + static_cast<unsigned char>(c);
            }
            int bucket = static_cast<int>(((h % partitionInfo_.hashCount) + partitionInfo_.hashCount)
                                           % partitionInfo_.hashCount);
            return "p" + std::to_string(bucket);
        }
        return "";
    }

    // Returns partition stats: vector of {name, rowCount}
    std::vector<std::pair<std::string, size_t>> getPartitionStats() const {
        std::map<std::string, size_t> counts;
        if (partitionInfo_.type == PartitionType::RANGE) {
            for (auto& r : partitionInfo_.ranges) counts[r.name] = 0;
        } else if (partitionInfo_.type == PartitionType::LIST) {
            for (auto& l : partitionInfo_.lists) counts[l.name] = 0;
        } else if (partitionInfo_.type == PartitionType::HASH) {
            for (int i = 0; i < partitionInfo_.hashCount; ++i)
                counts["p" + std::to_string(i)] = 0;
        }
        for (auto& row : rows_) {
            std::string pn = getPartitionName(row);
            if (!pn.empty()) counts[pn]++;
        }
        std::vector<std::pair<std::string, size_t>> result;
        for (auto& kv : counts) result.push_back(kv);
        return result;
    }

    // Returns set of partition names that could contain rows matching col op val
    // (partition pruning). Returns all partition names if pruning not applicable.
    std::vector<std::string> prunePartitions(const std::string& col,
                                              const std::string& op,
                                              const std::string& val) const {
        std::vector<std::string> all;
        if (partitionInfo_.type == PartitionType::RANGE) {
            for (auto& r : partitionInfo_.ranges) all.push_back(r.name);
        } else if (partitionInfo_.type == PartitionType::LIST) {
            for (auto& l : partitionInfo_.lists) all.push_back(l.name);
        } else if (partitionInfo_.type == PartitionType::HASH) {
            for (int i = 0; i < partitionInfo_.hashCount; ++i)
                all.push_back("p" + std::to_string(i));
        }
        if (col != partitionInfo_.column) return all;
        // Strip surrounding quotes from WHERE value for consistent matching
        std::string cmpVal = val;
        if (cmpVal.size() >= 2 && cmpVal.front() == '\'' && cmpVal.back() == '\'')
            cmpVal = cmpVal.substr(1, cmpVal.size() - 2);
        if (partitionInfo_.type == PartitionType::RANGE) {
            long long numVal = 0;
            try { numVal = std::stoll(cmpVal); } catch (...) { return all; }
            std::vector<std::string> pruned;
            long long prevLimit = std::numeric_limits<long long>::min();
            for (auto& r : partitionInfo_.ranges) {
                // partition covers [prevLimit, r.limit)
                bool keep = false;
                if (op == "=" || op == "BETWEEN") {
                    keep = (numVal >= prevLimit && numVal < r.limit);
                } else if (op == "<" || op == "<=") {
                    keep = (prevLimit < numVal + (op == "<=" ? 1 : 0));
                } else if (op == ">" || op == ">=") {
                    keep = (r.limit > numVal);
                } else {
                    keep = true;
                }
                if (keep) pruned.push_back(r.name);
                prevLimit = r.limit;
            }
            return pruned.empty() ? all : pruned;
        }
        if (partitionInfo_.type == PartitionType::LIST && op == "=") {
            for (auto& l : partitionInfo_.lists) {
                for (auto& v : l.values) {
                    if (v == cmpVal) return {l.name};
                }
            }
            return {};
        }
        if (partitionInfo_.type == PartitionType::HASH && op == "=") {
            long long h = 0;
            try { h = std::stoll(cmpVal); } catch (...) {
                for (char c : cmpVal) h = h * 31 + static_cast<unsigned char>(c);
            }
            int bucket = static_cast<int>(((h % partitionInfo_.hashCount) + partitionInfo_.hashCount)
                                           % partitionInfo_.hashCount);
            return {"p" + std::to_string(bucket)};
        }
        return all;
    }

private:
    std::string         name_;
    std::vector<Column> columns_;
    std::vector<Row>    rows_;
    std::map<std::string, IndexEntry>  indices_;
    std::map<std::string, uint64_t>    autoIncMap_;      // colName → nächster Wert
    std::vector<ForeignKeyDef>         foreignKeys_;     // FK-Definitionen
    PartitionInfo                      partitionInfo_;   // Phase 62: Partition metadata

    void rebuildAll() {
        for (auto& [idxName, entry] : indices_) {
            if (entry.cols.empty()) continue;
            int ci = colOf(entry.cols[0]);
            if (ci < 0) continue;
            entry.tree->clear();
            for (size_t ri = 0; ri < rows_.size(); ++ri)
                if (rows_[ri].xmax == 0 &&  // Phase 71: skip dead rows
                    static_cast<size_t>(ci) < rows_[ri].values.size())
                    entry.tree->insert(rows_[ri].values[ci], ri);
        }
    }
};

// Phase 80: Column Store Engine (included here so ColumnTable sees Column/Row/Table/WhereCondition)
#include "../storage/column_store.hpp"

// Phase 83: Join Algorithms (included here inside namespace milansql so they see Row/Table/Column)
#include "../executor/join_planner.hpp"
#include "../executor/hash_join.hpp"
#include "../executor/merge_join.hpp"

// Phase 84: Page-based I/O (included here so they see Row/Table/Column)
#include "../storage/page.hpp"
#include "../storage/page_manager.hpp"
#include "../storage/paged_table.hpp"

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

// ── Phase 81: Logical Replication Structs ────────────────────
struct PublicationDef {
    std::string name;
    std::vector<std::string> tables;  // empty = all tables
    bool allTables = false;
};

struct SubscriptionDef {
    std::string name;
    std::string connection;    // DSN string
    std::string publication;   // publication name on master
    bool enabled = true;
};

// ------------------------------------------------------------
// Engine
// ------------------------------------------------------------
class Engine {
public:
    Engine() {
        loadPublications_();
        loadSubscriptions_();
        loadPagedTableSchemas_();
    }

    // ── Phase 75: Row-Level Security ──────────────────────────
    struct RlsPolicy {
        std::string name;
        std::string table;
        std::string command;
        std::string role;
        std::string usingExpr;
    };

    // ── DDL ───────────────────────────────────────────────────

    void createTable(const std::string& name, std::vector<Column> cols,
                     std::vector<ForeignKeyDef> fks = {},
                     const std::string& inherits = "") {
        auto key = resolveTableName(name);
        if (tables_.count(key))
            throw std::runtime_error("Tabelle '" + key + "' existiert bereits.");
        if (!inherits.empty()) {
            // Phase 78: INHERITS — prepend parent columns to child columns
            auto parentKey = resolveTableName(inherits);
            if (!tables_.count(parentKey))
                throw std::runtime_error(
                    "Eltern-Tabelle '" + parentKey + "' nicht gefunden.");
            const auto& parentCols = tables_.at(parentKey).columns();
            std::vector<Column> allCols(parentCols.begin(), parentCols.end());
            for (const auto& c : cols) allCols.push_back(c);
            tables_.emplace(key, Table(key, std::move(allCols)));
            tableParent_[key] = parentKey;
            tableChildren_[parentKey].push_back(key);
        } else {
            tables_.emplace(key, Table(key, std::move(cols)));
        }
        for (const auto& fk : fks)
            tables_.at(key).addForeignKey(fk);
    }

    void createIndex(const std::string& tbl,
                     const std::vector<std::string>& cols,
                     const std::string& idxName,
                     const std::string& idxType = "BTREE") {
        getTable(resolveTableName(tbl)).createIndex(cols, idxName, idxType);
    }

    void dropIndex(const std::string& tbl, const std::string& idxName) {
        getTable(resolveTableName(tbl)).dropIndex(idxName);
    }

    void dropTable(const std::string& name) {
        auto key = resolveTableName(name);
        if (!tables_.erase(key))
            throw std::runtime_error("Tabelle '" + key + "' nicht gefunden.");
        // Phase 78: clean up inheritance maps
        auto pit = tableParent_.find(key);
        if (pit != tableParent_.end()) {
            auto& siblings = tableChildren_[pit->second];
            siblings.erase(std::remove(siblings.begin(), siblings.end(), key), siblings.end());
            tableParent_.erase(pit);
        }
        tableChildren_.erase(key);
    }

    // AUTO_INCREMENT-Zähler setzen (wird beim Laden aus Datei aufgerufen)
    void setTableAutoInc(const std::string& tbl,
                         const std::string& col, uint64_t val) {
        getTable(resolveTableName(tbl)).setAutoInc(col, val);
    }

    // FOREIGN KEY hinzufügen (wird beim Laden aus Datei aufgerufen)
    void addForeignKey(const std::string& tbl, const ForeignKeyDef& fk) {
        getTable(resolveTableName(tbl)).addForeignKey(fk);
    }

    // ── DML ───────────────────────────────────────────────────

    void insertRow(const std::string& tblRaw, std::vector<std::string> vals) {
        auto tbl = resolveTableName(tblRaw);
        checkPrivilege("INSERT", tbl);  // Phase 46: access control
        if (!g_lockManager.checkWriteAllowed(tbl))  // Phase 65: table lock check
            throw std::runtime_error("Tabelle '" + tbl + "' ist durch LOCK TABLE READ gesperrt.");
        WriteScope ws(getOrCreateRwLock(tbl), tbl);  // Phase 112: exclusive write lock
        Table& t = getTable(tbl);       // wirft wenn Tabelle fehlt
        applyDefaults(t, vals);         // fehlende Werte mit DEFAULT/NULL füllen
        applyAutoInc(t, vals);          // AUTO_INCREMENT-Spalten befüllen
        applyGeneratedCols(t, vals);    // Phase 68: Generated Columns berechnen
        checkInsertFK(tbl, vals);       // FOREIGN KEY prüfen
        checkAllConstraints(tbl, vals); // Phase 23: CHECK constraints prüfen
        checkJsonColumns(t, vals);      // Phase 56: JSON-Validierung

        // Phase 43: BEFORE INSERT triggers
        {
            std::string signalMsg;
            std::vector<std::string> emptyOld;
            if (!fireAllTriggers("BEFORE", "INSERT", tbl, vals, emptyOld, signalMsg))
                throw std::runtime_error(signalMsg);
        }

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
        t.insert(Row(vals));
        bufferPool_.markDirty(tbl);  // Phase 73: mark page dirty

        // Phase 49: Update fulltext indexes for this table
        for (auto& [n, fi] : fulltextIndices_) {
            if (fi.tableName == tbl) {
                buildFulltextIndex(fi, tables_[tbl]);
            }
        }

        // Phase 43: AFTER INSERT triggers
        {
            std::string signalMsg;
            std::vector<std::string> emptyOld;
            fireAllTriggers("AFTER", "INSERT", tbl, vals, emptyOld, signalMsg);
        }

        // Phase 81: logical replication log
        {
            auto key = resolveTableName(tblRaw);
            if (tables_.count(key)) {
                const auto& cols_ref = tables_.at(key).columns();
                std::vector<std::string> colNames;
                for (const auto& c : cols_ref) colNames.push_back(c.name);
                logicalLog_WriteIfPublished_(key, "INSERT", colNames, vals);
            }
        }

        // Phase 98: CDC hook for INSERT
        {
            auto key = resolveTableName(tblRaw);
            if (cdcMgr_.isEnabled(key) && tables_.count(key)) {
                const auto& cols_ref = tables_.at(key).columns();
                std::vector<std::string> colNames;
                for (const auto& c : cols_ref) colNames.push_back(c.name);
                cdcMgr_.recordInsert(key, colNames, vals);
            }
        }
    }

    // ── Phase 39: UPSERT ──────────────────────────────────────

    // INSERT OR REPLACE: löscht kollidierte Zeilen, fügt neue ein.
    // Gibt true zurück wenn mind. eine Zeile ersetzt wurde, false bei normalem Insert.
    bool insertOrReplace(const std::string& tblRaw, std::vector<std::string> vals) {
        auto tbl = resolveTableName(tblRaw);
        checkPrivilege("INSERT", tbl);  // Phase 46: access control
        if (inTransaction_)
            throw std::runtime_error("INSERT OR REPLACE nicht innerhalb einer Transaktion.");
        Table& t = getTable(tbl);
        applyDefaults(t, vals);
        applyAutoInc(t, vals);
        applyGeneratedCols(t, vals);    // Phase 68
        checkInsertFK(tbl, vals);
        checkAllConstraints(tbl, vals);
        auto conflicts = t.conflictRows(vals);
        bool replaced = !conflicts.empty();
        if (replaced) t.eraseByIndices(conflicts);
        t.insert(Row(vals));   // UNIQUE-Check nach Löschung sauber
        return replaced;
    }

    // INSERT OR IGNORE: ignoriert den Insert bei PK/UNIQUE-Konflikt (kein Fehler).
    // Gibt true zurück wenn eingefügt, false wenn ignoriert.
    bool insertOrIgnore(const std::string& tblRaw, std::vector<std::string> vals) {
        auto tbl = resolveTableName(tblRaw);
        checkPrivilege("INSERT", tbl);  // Phase 46: access control
        if (inTransaction_)
            throw std::runtime_error("INSERT OR IGNORE nicht innerhalb einer Transaktion.");
        Table& t = getTable(tbl);
        applyDefaults(t, vals);
        applyAutoInc(t, vals);
        applyGeneratedCols(t, vals);    // Phase 68
        checkInsertFK(tbl, vals);
        if (!t.conflictRows(vals).empty()) return false;  // ignorieren
        t.insert(Row(vals));
        return true;
    }

    struct WhereResult {
        Table table;
        bool  usedIndex;
    };

    const Table& selectAll(const std::string& tbl) const {
        auto key = resolveTableName(tbl);
        bufferPool_.access(key);  // Phase 73: Buffer Pool tracking
        return getTable(key);
    }

    // Phase 75/78: selectAll with RLS + inheritance (returns Table by value)
    Table selectAllFiltered(const std::string& tbl, bool fromOnly = false) const {
        auto key = resolveTableName(tbl);
        bufferPool_.access(key);
        ReadScope rs(getOrCreateRwLock(key), key);  // Phase 112: shared read lock
        const auto& src = getTable(key);
        Table result = src.clone();
        result.mutableRows() = applyRls_(key, src.rows(), "SELECT");
        // Phase 78: include rows from child tables (projected to parent columns)
        if (!fromOnly) {
            size_t parentColCount = src.columns().size();
            collectInheritedRows_(key, parentColCount, {}, {}, "AND", result, true);
        }
        return result;
    }

    // SELECT mit WHERE (AND / OR, alle Operatoren inkl. LIKE)
    WhereResult selectWhere(const std::string& tblNameRaw,
                            const std::vector<WhereCondition>& conds,
                            const std::string& logic = "AND",
                            bool fromOnly = false) const {
        auto tblName = resolveTableName(tblNameRaw);
        bufferPool_.access(tblName);  // Phase 73: Buffer Pool tracking
        ReadScope rs(getOrCreateRwLock(tblName), tblName);  // Phase 112: shared read lock
        const Table& src = getTable(tblName);
        Table result(tblName, src.columns());
        bool usedIndex = false;

        if (conds.empty()) {
            for (const auto& row : src.rows()) {
                if (row.xmax != 0) continue;  // Phase 71: skip dead rows
                result.insert(row);
            }
            // Phase 75: RLS
            {
                auto filtered = applyRls_(tblName, result.rows(), "SELECT");
                if (filtered.size() != result.rows().size()) {
                    Table rlsResult(tblName, result.columns());
                    for (const auto& r : filtered) rlsResult.insert(r);
                    return {std::move(rlsResult), false};
                }
            }
            return {std::move(result), false};
        }

        // Index nur bei einzelner "="-Bedingung
        if (conds.size() == 1 && conds[0].op == "=" && src.hasIndex(conds[0].col)) {
            usedIndex = true;
            for (size_t ri : src.indexSearch(conds[0].col, conds[0].val))
                if (ri < src.rows().size() && src.rows()[ri].xmax == 0)  // Phase 71
                    result.insert(src.rows()[ri]);
            // Phase 75: RLS
            {
                auto filtered = applyRls_(tblName, result.rows(), "SELECT");
                if (filtered.size() != result.rows().size()) {
                    Table rlsResult(tblName, result.columns());
                    for (const auto& r : filtered) rlsResult.insert(r);
                    return {std::move(rlsResult), usedIndex};
                }
            }
            return {std::move(result), usedIndex};
        }

        // EXISTS/NOT EXISTS + korrelierte Scalar Subqueries + CAST-LHS + MATCH AGAINST brauchen rowMatches
        bool hasCorrelated = false;
        for (const auto& cond : conds) {
            if (cond.op == "EXISTS" || cond.op == "NOT EXISTS") { hasCorrelated = true; break; }
            if (cond.isScalarSub)    { hasCorrelated = true; break; }
            if (cond.isFuncLhs)      { hasCorrelated = true; break; }
            if (cond.isMatchAgainst) { hasCorrelated = true; break; }
        }

        if (hasCorrelated) {
            for (const auto& row : src.rows()) {
                if (row.xmax != 0) continue;  // Phase 71: skip dead rows
                if (rowMatches(src, row, conds, logic)) result.insert(row);
            }
            // Phase 75: RLS
            {
                auto filtered = applyRls_(tblName, result.rows(), "SELECT");
                if (filtered.size() != result.rows().size()) {
                    Table rlsResult(tblName, result.columns());
                    for (const auto& r : filtered) rlsResult.insert(r);
                    return {std::move(rlsResult), false};
                }
            }
            return {std::move(result), false};
        }

        std::vector<std::size_t> cis;
        cis.reserve(conds.size());
        for (const auto& cond : conds)
            cis.push_back(colIdx(src, cond.col));

        // Phase 77: parallel scan when rows >= threshold
        {
            const auto& allRows = src.rows();
            bool useParallel = parallelExec_.numWorkers > 1 &&
                               (long long)allRows.size() >= parallelExec_.threshold;
            if (useParallel) {
                auto pf = parallelFilter_(allRows, conds, cis, logic,
                                          parallelExec_.numWorkers);
                for (const auto& r : pf) result.insert(r);
            } else {
                for (const auto& row : allRows) {
                    if (row.xmax != 0) continue;  // Phase 71: skip dead rows
                    if (rowMatchesByIdx(row, conds, cis, logic))
                        result.insert(row);
                }
            }
        }
        // Phase 78: include matching rows from child tables (WHERE on parent cols)
        if (!fromOnly) {
            collectInheritedRows_(tblName, src.columns().size(),
                                  conds, cis, logic, result, false);
        }
        // Phase 75: apply Row-Level Security filter
        {
            auto filtered = applyRls_(tblName, result.rows(), "SELECT");
            if (filtered.size() != result.rows().size()) {
                Table rlsResult(tblName, result.columns());
                for (const auto& r : filtered) rlsResult.insert(r);
                return {std::move(rlsResult), usedIndex};
            }
        }
        return {std::move(result), usedIndex};
    }

    // COUNT(*) [mit optionalem WHERE]
    std::size_t countRows(const std::string& tbl, bool fromOnly = false) const {
        auto key = resolveTableName(tbl);
        size_t n = getTable(key).rowCount();
        // Phase 78: count child rows recursively
        if (!fromOnly) n += countInheritedRows_(key);
        return n;
    }

    std::size_t countWhere(const std::string& tblName,
                           const std::vector<WhereCondition>& conds,
                           const std::string& logic = "AND",
                           bool fromOnly = false) const {
        if (conds.empty()) return countRows(tblName, fromOnly);
        return selectWhere(tblName, conds, logic, fromOnly).table.rowCount();
    }

    // Aggregatfunktion ohne GROUP BY
    std::string computeAggregate(const std::string& tblNameRaw,
                                  const std::string& func,
                                  const std::string& col,
                                  const std::vector<WhereCondition>& conds,
                                  const std::string& logic = "AND") const {
        auto tblName = resolveTableName(tblNameRaw);
        const Table& src = getTable(tblName);
        std::size_t ci = colIdx(src, col);

        // Phase 77: parallel aggregate (COUNT uses countRows path, not here)
        if (parallelExec_.numWorkers > 1 &&
            (long long)src.rows().size() >= parallelExec_.threshold &&
            conds.empty()) {
            return parallelAggregate_(src, func, ci, parallelExec_.numWorkers);
        }

        std::vector<double> nums;
        for (const auto& row : src.rows()) {
            if (row.xmax != 0) continue;  // Phase 71: skip dead rows
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
    Table groupBy(const std::string& tblNameRaw,
                  const std::vector<WhereCondition>& whereConds,
                  const std::string& whereLogic,
                  const std::vector<std::string>& groupCols,
                  const std::vector<SelectItem>& selectItems,
                  const std::vector<HavingCondition>& havingConds,
                  const std::string& havingLogic) const {

        auto tblName = resolveTableName(tblNameRaw);
        const Table& src = getTable(tblName);

        if (groupCols.empty())
            throw std::runtime_error("GROUP BY: keine Spalten angegeben.");
        if (selectItems.empty())
            throw std::runtime_error("GROUP BY: leere SELECT-Liste.");

        // Step 1 — WHERE-Filter: indizes der passenden Zeilen sammeln
        std::vector<size_t> filtered;
        filtered.reserve(src.rows().size());
        for (size_t i = 0; i < src.rows().size(); ++i) {
            if (src.rows()[i].xmax != 0) continue;  // Phase 71: skip dead rows
            if (rowMatches(src, src.rows()[i], whereConds, whereLogic))
                filtered.push_back(i);
        }

        // Step 2 — GROUP BY-Spaltenindizes vorab auflösen
        // For each GROUP BY column: find direct column index OR matching selectItem alias
        // (alias grouping, e.g. GROUP BY tag where tag = time_bucket(...) AS tag)
        struct GroupByKey {
            size_t ci = std::string::npos;  // direct column index (if valid)
            int selectItemIdx = -1;         // selectItem index for alias-based grouping
        };
        std::vector<GroupByKey> groupKeys;
        groupKeys.reserve(groupCols.size());
        for (const auto& gc : groupCols) {
            GroupByKey gk;
            // Try direct column lookup first
            bool found = false;
            for (size_t si = 0; si < src.columns().size(); ++si) {
                if (src.columns()[si].name == gc) {
                    gk.ci = si;
                    found = true;
                    break;
                }
            }
            if (!found) {
                // Try alias match in selectItems
                for (size_t si = 0; si < selectItems.size(); ++si) {
                    if (!selectItems[si].alias.empty() && selectItems[si].alias == gc) {
                        gk.selectItemIdx = static_cast<int>(si);
                        break;
                    }
                }
            }
            groupKeys.push_back(gk);
        }

        // Phase 97: compute per-row value for a selectItem (handles funcExpr like time_bucket)
        auto computeItemValue = [&](const SelectItem& item, const Row& row) -> std::string {
            if (item.isFuncExpr) {
                return evaluateFunc(item.funcName, item.funcArgs, src, row);
            }
            size_t ci2 = colIdx(src, item.colName);
            return ci2 < row.values.size() ? row.values[ci2] : "";
        };

        // Step 3 — Zeilen gruppieren (Reihenfolge des ersten Auftretens erhalten)
        std::map<std::string, std::vector<size_t>> groupMap;
        std::vector<std::string> groupOrder;

        for (size_t ri : filtered) {
            const Row& row = src.rows()[ri];
            // Schlüssel: Werte der GROUP BY-Spalten, durch \x01 getrennt
            std::string key;
            for (const auto& gk : groupKeys) {
                key += '\x01';
                if (gk.ci != std::string::npos) {
                    key += (gk.ci < row.values.size() ? row.values[gk.ci] : "");
                } else if (gk.selectItemIdx >= 0) {
                    key += computeItemValue(selectItems[static_cast<size_t>(gk.selectItemIdx)], row);
                }
            }
            if (!groupMap.count(key)) groupOrder.push_back(key);
            groupMap[key].push_back(ri);
        }

        // Step 4 — Ergebnis-Spalten aus SelectItems ableiten
        std::vector<Column> resultCols;
        resultCols.reserve(selectItems.size());
        for (const auto& item : selectItems) {
            std::string name;
            if (!item.alias.empty()) {
                name = item.alias;
            } else if (item.isAgg) {
                name = item.aggFunc + "(" + item.aggCol + ")";
            } else {
                name = item.colName;
            }
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
                } else if (item.isFuncExpr) {
                    // Phase 97: function expression (e.g. time_bucket) — evaluate on first row
                    const Row& firstRow = src.rows()[riList[0]];
                    vals.push_back(evaluateFunc(item.funcName, item.funcArgs, src, firstRow));
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
    Table innerJoin(const std::string& t1NameRaw, const std::string& t2NameRaw,
                    const std::string& onT1Col, const std::string& onT2Col,
                    const std::vector<WhereCondition>& whereConds,
                    const std::string& whereLogic) const {
        auto t1Name = resolveTableName(t1NameRaw);
        auto t2Name = resolveTableName(t2NameRaw);
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
            if (row1.xmax != 0) continue;  // Phase 71: skip dead rows
            const std::string& k1 =
                ci1 < row1.values.size() ? row1.values[ci1] : "";
            for (const auto& row2 : t2.rows()) {
                if (row2.xmax != 0) continue;  // Phase 71
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
    Table executeJoins(const std::string& baseNameRaw,
                       const std::vector<JoinClause>& joinsRaw,
                       const std::vector<WhereCondition>& whereConds,
                       const std::string& whereLogic) const {

        auto baseName = resolveTableName(baseNameRaw);
        // Resolve table names in join clauses
        std::vector<JoinClause> joins = joinsRaw;
        for (auto& jc : joins)
            jc.table = resolveTableName(jc.table);

        const Table& base = getTable(baseName);

        // Startergebnis: Spalten qualifizieren
        std::vector<Column> initCols;
        initCols.reserve(base.columns().size());
        for (const auto& c : base.columns())
            initCols.emplace_back(baseName + "." + c.name, c.type);

        Table current("", initCols);
        for (const auto& r : base.rows()) {
            if (r.xmax != 0) continue;  // Phase 71: skip dead rows
            current.insert(r);
        }

        // ── Phase 113: DP Join Order Optimizer ───────────────────
        // For 3+ tables, use bitmask DP to find optimal join order.
        if (joins.size() >= 2) {
            // Build per-table info
            std::vector<milansql::JoinTableInfo> dpTables;
            dpTables.push_back({baseName, base.rowCount(), {}});
            for (const auto& jc2 : joins) {
                milansql::JoinTableInfo jti;
                jti.name     = jc2.table;
                jti.rowCount = tables_.count(jc2.table)
                                ? tables_.at(jc2.table).rowCount() : 0;
                dpTables.push_back(jti);
            }

            // Helper: extract table prefix from "tbl.col"
            auto tblOf113 = [](const std::string& s) -> std::string {
                auto p = s.rfind('.'); return p != std::string::npos ? s.substr(0, p) : s;
            };

            // Helper: find which dpTables[] index matches a name/alias
            auto findIdx113 = [&](const std::string& pfx) -> int {
                if (pfx.empty()) return -1;
                for (int ti = 0; ti < static_cast<int>(dpTables.size()); ++ti) {
                    if (tblNamesMatch(pfx, dpTables[static_cast<size_t>(ti)].name)) return ti;
                }
                // Alias lookup for join tables
                for (int ti = 1; ti < static_cast<int>(dpTables.size()); ++ti) {
                    const JoinClause& jci = joins[static_cast<size_t>(ti - 1)];
                    if (!jci.tableAlias.empty() && pfx == jci.tableAlias) return ti;
                }
                return -1;  // unknown (likely base table alias)
            };

            // Compute requiredTables (connectivity) for each join table
            for (size_t ji = 1; ji < dpTables.size(); ++ji) {
                const JoinClause& jc2 = joins[ji - 1];
                std::string leftPfx   = tblOf113(jc2.onLeft);
                std::string rightPfx  = tblOf113(jc2.onRight);

                std::set<int> req;
                for (const std::string& pfx : {leftPfx, rightPfx}) {
                    int idx = findIdx113(pfx);
                    if (idx < 0) {
                        req.insert(0);  // unknown alias → assume base table
                    } else if (idx != static_cast<int>(ji)) {
                        req.insert(idx);
                    }
                }
                if (req.empty()) req.insert(0);
                dpTables[ji].requiredTables.assign(req.begin(), req.end());
            }

            // Build cache key from sorted table names
            std::string cacheKey;
            std::vector<std::string> tNames;
            for (const auto& t : dpTables) tNames.push_back(t.name);
            std::sort(tNames.begin(), tNames.end());
            for (const auto& n : tNames) cacheKey += n + ",";

            // Run DP planner
            auto dpResult = milansql::g_dpPlanner().plan(dpTables, cacheKey);
            lastJoinPlan_ = dpResult;

            // Reorder joins if DP found a valid plan
            if (dpResult.dpUsed &&
                dpResult.joinOrder.size() == joins.size()) {
                std::vector<JoinClause> reordered;
                reordered.reserve(joins.size());
                for (int jcIdx : dpResult.joinOrder)
                    reordered.push_back(joins[static_cast<size_t>(jcIdx)]);
                joins = std::move(reordered);
            }
        }

        // Jeden JOIN-Schritt nacheinander anwenden
        for (const auto& jc : joins) {
            const Table& right = getTable(jc.table);

            // Neues Schema: bisherige Spalten + neue qualifizierte Spalten
            std::vector<Column> newCols = current.columns();
            for (const auto& c : right.columns())
                newCols.emplace_back(jc.table + "." + c.name, c.type);

            // ON-Seiten zuordnen: eine Seite gehört zur rechten Tabelle, die andere
            // zum bisherigen Ergebnis. Seiten ggf. tauschen.
            // For schema-qualified refs like "shop.kunden.id", table = "shop.kunden"
            auto tblOf = [](const std::string& s) -> std::string {
                auto p = s.rfind('.');  // use LAST dot to separate table.col
                return p != std::string::npos ? s.substr(0, p) : "";
            };
            std::string resultSide = jc.onLeft;
            std::string rightSide  = jc.onRight;
            // Use tblNamesMatch to handle bare vs schema-qualified names
            if (tblNamesMatch(tblOf(jc.onLeft), jc.table))
                std::swap(resultSide, rightSide);

            size_t leftCI  = findQualColIdx(current, resultSide);
            size_t rightCI = findRawColIdx (right,   rightSide);

            // ── Phase 83: Choose join algorithm ──────────────────────────
            auto bareColOf83 = [](const std::string& s) -> std::string {
                auto p = s.rfind('.'); return p != std::string::npos ? s.substr(p+1) : s;
            };
            std::string rightColBare83 = bareColOf83(rightSide);
            bool rightHasIdx83 = right.hasIndex(rightColBare83);
            bool leftHasIdx83  = false;
            {
                auto p2 = resultSide.rfind('.');
                if (p2 != std::string::npos) {
                    std::string lTbl83 = resultSide.substr(0, p2);
                    if (tables_.count(lTbl83))
                        leftHasIdx83 = tables_.at(lTbl83).hasIndex(bareColOf83(resultSide));
                }
            }
            JoinStrategy strategy83 = JoinPlanner::choose(
                current.rowCount(), right.rowCount(), leftHasIdx83, rightHasIdx83);

            // Dispatch: Hash Join or Merge Join for large tables;
            // Merge Join only supports INNER — fall back to Hash Join for others.
            if (strategy83 == JoinStrategy::HASH_JOIN ||
                (strategy83 == JoinStrategy::MERGE_JOIN && jc.joinType != "INNER")) {
                current = HashJoin::execute(current, right, leftCI, rightCI, jc.joinType, newCols);
            } else if (strategy83 == JoinStrategy::MERGE_JOIN) {
                current = MergeJoin::execute(current, right, leftCI, rightCI, jc.joinType, newCols);
            } else {
                // ── NESTED LOOP (original algorithm for small tables) ─────
                Table next("", newCols);
                const size_t leftWidth  = current.columns().size();
                const size_t rightWidth = right.columns().size();

                if (jc.joinType == "INNER" || jc.joinType == "LEFT") {
                    // ── INNER JOIN / LEFT JOIN ────────────────────
                    for (const auto& lrow : current.rows()) {
                        const std::string& lval =
                            leftCI < lrow.values.size() ? lrow.values[leftCI] : "";
                        bool matched = false;

                        for (const auto& rrow : right.rows()) {
                            const std::string& rval =
                                rightCI < rrow.values.size() ? rrow.values[rightCI] : "";
                            if (lval != rval) continue;

                            std::vector<std::string> vals;
                            vals.reserve(leftWidth + rrow.values.size());
                            vals.insert(vals.end(), lrow.values.begin(), lrow.values.end());
                            vals.insert(vals.end(), rrow.values.begin(), rrow.values.end());
                            next.insert(Row(vals));
                            matched = true;
                        }

                        if (!matched && jc.joinType == "LEFT") {
                            std::vector<std::string> vals;
                            vals.reserve(leftWidth + rightWidth);
                            vals.insert(vals.end(), lrow.values.begin(), lrow.values.end());
                            for (size_t k = 0; k < rightWidth; ++k) vals.push_back("NULL");
                            next.insert(Row(vals));
                        }
                    }

                } else if (jc.joinType == "RIGHT") {
                    // ── RIGHT JOIN ────────────────────────────────
                    for (const auto& rrow : right.rows()) {
                        const std::string& rval =
                            rightCI < rrow.values.size() ? rrow.values[rightCI] : "";
                        bool matched = false;

                        for (const auto& lrow : current.rows()) {
                            const std::string& lval =
                                leftCI < lrow.values.size() ? lrow.values[leftCI] : "";
                            if (lval != rval) continue;

                            std::vector<std::string> vals;
                            vals.reserve(leftWidth + rrow.values.size());
                            vals.insert(vals.end(), lrow.values.begin(), lrow.values.end());
                            vals.insert(vals.end(), rrow.values.begin(), rrow.values.end());
                            next.insert(Row(vals));
                            matched = true;
                        }

                        if (!matched) {
                            std::vector<std::string> vals;
                            vals.reserve(leftWidth + rightWidth);
                            for (size_t k = 0; k < leftWidth;  ++k) vals.push_back("NULL");
                            vals.insert(vals.end(), rrow.values.begin(), rrow.values.end());
                            next.insert(Row(vals));
                        }
                    }

                } else if (jc.joinType == "FULL") {
                    // ── FULL OUTER JOIN ───────────────────────────
                    for (const auto& lrow : current.rows()) {
                        const std::string& lval =
                            leftCI < lrow.values.size() ? lrow.values[leftCI] : "";
                        bool matched = false;

                        for (const auto& rrow : right.rows()) {
                            const std::string& rval =
                                rightCI < rrow.values.size() ? rrow.values[rightCI] : "";
                            if (lval != rval) continue;

                            std::vector<std::string> vals;
                            vals.reserve(leftWidth + rrow.values.size());
                            vals.insert(vals.end(), lrow.values.begin(), lrow.values.end());
                            vals.insert(vals.end(), rrow.values.begin(), rrow.values.end());
                            next.insert(Row(vals));
                            matched = true;
                        }

                        if (!matched) {
                            std::vector<std::string> vals;
                            vals.reserve(leftWidth + rightWidth);
                            vals.insert(vals.end(), lrow.values.begin(), lrow.values.end());
                            for (size_t k = 0; k < rightWidth; ++k) vals.push_back("NULL");
                            next.insert(Row(vals));
                        }
                    }

                    // Anti-Right: rechte Zeilen ohne Match
                    for (const auto& rrow : right.rows()) {
                        const std::string& rval =
                            rightCI < rrow.values.size() ? rrow.values[rightCI] : "";
                        bool matched = false;
                        for (const auto& lrow : current.rows()) {
                            const std::string& lval =
                                leftCI < lrow.values.size() ? lrow.values[leftCI] : "";
                            if (lval == rval) { matched = true; break; }
                        }
                        if (!matched) {
                            std::vector<std::string> vals;
                            vals.reserve(leftWidth + rightWidth);
                            for (size_t k = 0; k < leftWidth; ++k) vals.push_back("NULL");
                            vals.insert(vals.end(), rrow.values.begin(), rrow.values.end());
                            next.insert(Row(vals));
                        }
                    }
                }

                current = std::move(next);
            }
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
    void alterTable(const std::string& tblNameRaw,
                    const std::string& op,
                    const std::string& colName,
                    const std::string& colType,    // nur für ADD
                    const std::string& newName) {  // nur für RENAME
        auto tblName = resolveTableName(tblNameRaw);
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
            const std::string& tblNameRaw,
            const std::string& col,
            const std::vector<WhereCondition>& conds,
            const std::string& logic) const {
        auto tblName = resolveTableName(tblNameRaw);
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

    std::size_t updateWhere(const std::string& tblRaw,
                            const std::string& setCol, const std::string& setVal,
                            const std::string& wCol,   const std::string& wVal) {
        auto tbl = resolveTableName(tblRaw);
        checkPrivilege("UPDATE", tbl);  // Phase 46: access control
        if (!g_lockManager.checkWriteAllowed(tbl))  // Phase 65: table lock check
            throw std::runtime_error("Tabelle '" + tbl + "' ist durch LOCK TABLE READ gesperrt.");
        WriteScope ws(getOrCreateRwLock(tbl), tbl);  // Phase 112: exclusive write lock
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
        auto n = t.updateWhere(colIdx(t, setCol), setVal, colIdx(t, wCol), wVal);
        bufferPool_.markDirty(tbl);  // Phase 73
        return n;
    }

    std::size_t updateAll(const std::string& tblRaw,
                          const std::string& setCol, const std::string& setVal) {
        auto tbl = resolveTableName(tblRaw);
        checkPrivilege("UPDATE", tbl);  // Phase 46: access control
        if (!g_lockManager.checkWriteAllowed(tbl))  // Phase 65: table lock check
            throw std::runtime_error("Tabelle '" + tbl + "' ist durch LOCK TABLE READ gesperrt.");
        WriteScope ws(getOrCreateRwLock(tbl), tbl);  // Phase 112: exclusive write lock
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
        auto n = t.updateAll(colIdx(t, setCol), setVal);
        bufferPool_.markDirty(tbl);  // Phase 73
        return n;
    }

    std::size_t deleteWhere(const std::string& tblRaw,
                            const std::string& wCol, const std::string& wVal) {
        auto tbl = resolveTableName(tblRaw);
        checkPrivilege("DELETE", tbl);  // Phase 46: access control
        if (!g_lockManager.checkWriteAllowed(tbl))  // Phase 65: table lock check
            throw std::runtime_error("Tabelle '" + tbl + "' ist durch LOCK TABLE READ gesperrt.");
        WriteScope ws(getOrCreateRwLock(tbl), tbl);  // Phase 112: exclusive write lock
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

        // Phase 43: Collect matched rows for BEFORE/AFTER DELETE triggers
        std::vector<std::vector<std::string>> matchedRows;
        {
            const Table& t = getTable(tbl);
            size_t wCI = colIdx(t, wCol);
            for (const auto& row : t.rows())
                if (wCI < row.values.size() && row.values[wCI] == wVal)
                    matchedRows.push_back(row.values);
        }

        // Phase 43: BEFORE DELETE triggers
        for (auto& rowVals : matchedRows) {
            std::string signalMsg;
            std::vector<std::string> emptyNew;
            if (!fireAllTriggers("BEFORE", "DELETE", tbl, emptyNew, rowVals, signalMsg))
                throw std::runtime_error(signalMsg);
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
        std::size_t deleted = t.deleteWhere(colIdx(t, wCol), wVal);
        if (deleted) vacuumMgr_.addDeadTuples(tbl, deleted);  // Phase 85
        bufferPool_.markDirty(tbl);  // Phase 73

        // Phase 49: Update fulltext indexes for this table
        for (auto& [n, fi] : fulltextIndices_) {
            if (fi.tableName == tbl) {
                buildFulltextIndex(fi, tables_[tbl]);
            }
        }

        // Phase 43: AFTER DELETE triggers
        for (auto& rowVals : matchedRows) {
            std::string signalMsg;
            std::vector<std::string> emptyNew;
            fireAllTriggers("AFTER", "DELETE", tbl, emptyNew, rowVals, signalMsg);
        }

        // Phase 98: CDC hook for DELETE
        if (deleted > 0 && cdcMgr_.isEnabled(tbl)) {
            const auto& t_ref = getTable(tbl);
            std::vector<std::string> colNames;
            for (const auto& c : t_ref.columns()) colNames.push_back(c.name);
            for (auto& rowVals : matchedRows)
                cdcMgr_.recordDelete(tbl, colNames, rowVals);
        }

        return deleted;
    }

    // Phase 22: Multi-Column UPDATE WHERE
    std::size_t updateWhere(const std::string& tblRaw,
                            const std::vector<std::string>& setCols,
                            const std::vector<std::string>& setVals,
                            const std::string& wCol, const std::string& wVal) {
        auto tbl = resolveTableName(tblRaw);
        checkPrivilege("UPDATE", tbl);  // Phase 46: access control
        WriteScope ws(getOrCreateRwLock(tbl), tbl);  // Phase 112: exclusive write lock
        // Phase 44: Check if any setVal contains an expression (col op num)
        // If so, do per-row evaluation
        bool hasExpr = false;
        Table& tblRef = getTable(tbl);
        for (const auto& val : setVals) {
            std::vector<std::string> vtoks;
            std::istringstream vss(val);
            std::string vt;
            while (vss >> vt) vtoks.push_back(vt);
            if (vtoks.size() >= 2) { hasExpr = true; break; }
        }

        if (hasExpr) {
            // Per-row update with expression evaluation
            size_t wCI = colIdx(tblRef, wCol);
            std::vector<std::size_t> setCIs;
            for (const auto& col : setCols)
                setCIs.push_back(colIdx(tblRef, col));
            std::size_t n = 0;
            for (auto& row : tblRef.mutableRows()) {
                if (wCI < row.values.size() && row.values[wCI] == wVal) {
                    for (size_t k = 0; k < setCIs.size() && k < setVals.size(); ++k) {
                        if (setCIs[k] < row.values.size()) {
                            std::string resolved = evalSetExpr(setVals[k], tblRef.columns(), row);
                            row.values[setCIs[k]] = resolved;
                        }
                    }
                    // Phase 68: recompute generated cols after row update
                    applyGeneratedCols(tblRef, row.values);
                    ++n;
                }
            }
            if (n) tblRef.rebuildIndexes();
            return n;
        }

        checkSetConstraints(tbl, setCols, setVals);  // Phase 23

        if (inTransaction_) {
            // In Transaktion: als mehrere Einzel-Ops puffern
            for (size_t k = 0; k < setCols.size() && k < setVals.size(); ++k)
                updateWhere(tbl, setCols[k], setVals[k], wCol, wVal);
            return 0;
        }

        size_t wCI = colIdx(tblRef, wCol);

        // Phase 43: collect old rows and compute new rows for BEFORE UPDATE triggers
        std::vector<std::size_t> setCIs2;
        for (const auto& col : setCols)
            setCIs2.push_back(colIdx(tblRef, col));

        std::vector<std::vector<std::string>> oldRows;
        for (const auto& row : tblRef.rows())
            if (wCI < row.values.size() && row.values[wCI] == wVal)
                oldRows.push_back(row.values);

        // Fire BEFORE UPDATE triggers
        for (auto& oldVals : oldRows) {
            // Compute proposed newRow
            std::vector<std::string> newVals = oldVals;
            for (size_t k = 0; k < setCIs2.size() && k < setVals.size(); ++k)
                if (setCIs2[k] < newVals.size())
                    newVals[setCIs2[k]] = setVals[k];

            std::string signalMsg;
            if (!fireAllTriggers("BEFORE", "UPDATE", tbl, newVals, oldVals, signalMsg))
                throw std::runtime_error(signalMsg);
        }

        std::size_t n = tblRef.updateWhere(setCIs2, setVals, wCI, wVal);

        // Phase 68: recompute generated cols for affected rows
        for (auto& row : tblRef.mutableRows())
            if (wCI < row.values.size() && row.values[wCI] == wVal)
                applyGeneratedCols(tblRef, row.values);
        if (n) tblRef.rebuildIndexes();

        // Phase 49: Update fulltext indexes for this table
        for (auto& [fn, fi] : fulltextIndices_) {
            if (fi.tableName == tbl) {
                buildFulltextIndex(fi, tables_[tbl]);
            }
        }

        // Phase 43: AFTER UPDATE triggers
        for (auto& oldVals : oldRows) {
            std::vector<std::string> newVals = oldVals;
            for (size_t k = 0; k < setCIs2.size() && k < setVals.size(); ++k)
                if (setCIs2[k] < newVals.size())
                    newVals[setCIs2[k]] = setVals[k];

            std::string signalMsg;
            fireAllTriggers("AFTER", "UPDATE", tbl, newVals, oldVals, signalMsg);
        }

        // Phase 98: CDC hook for UPDATE
        if (n > 0 && cdcMgr_.isEnabled(tbl)) {
            const auto& cols_ref = tblRef.columns();
            std::vector<std::string> colNames;
            for (const auto& c : cols_ref) colNames.push_back(c.name);
            for (auto& oldVals : oldRows) {
                std::vector<std::string> newVals2 = oldVals;
                for (size_t k = 0; k < setCIs2.size() && k < setVals.size(); ++k)
                    if (setCIs2[k] < newVals2.size())
                        newVals2[setCIs2[k]] = setVals[k];
                cdcMgr_.recordUpdate(tbl, colNames, oldVals, newVals2);
            }
        }

        return n;
    }

    // Phase 22: Multi-Column UPDATE ALL
    std::size_t updateAll(const std::string& tblRaw,
                          const std::vector<std::string>& setCols,
                          const std::vector<std::string>& setVals) {
        auto tbl = resolveTableName(tblRaw);
        checkPrivilege("UPDATE", tbl);  // Phase 46: access control
        WriteScope ws(getOrCreateRwLock(tbl), tbl);  // Phase 112: exclusive write lock
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
        std::size_t n = t.updateAll(setCIs, setVals);
        // Phase 68: recompute generated cols for all rows
        for (auto& row : t.mutableRows())
            applyGeneratedCols(t, row.values);
        if (n) t.rebuildIndexes();
        return n;
    }

    std::size_t deleteAll(const std::string& tblRaw) {
        auto tbl = resolveTableName(tblRaw);
        checkPrivilege("DELETE", tbl);  // Phase 46: access control
        WriteScope ws(getOrCreateRwLock(tbl), tbl);  // Phase 112: exclusive write lock
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
    void truncateTable(const std::string& tblRaw) {
        auto tbl = resolveTableName(tblRaw);
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
        // Phase 71: MVCC — start versioned transaction
        // Phase 72: Write TX_BEGIN marker (WAL cleared at startup by recovery, not here)
        mvccTxId_ = txManager_.beginTx(isolationLevel_);
        {
            std::ofstream wal(walPath_, std::ios::app);
            if (wal) wal << "TX_BEGIN:" << mvccTxId_ << "\n";
        }
        inTransaction_ = true;
        txBuffer_.clear();
        savepointStack_.clear();         // Phase 64
    }

    // COMMIT: alle gepufferten Ops auf Tabellen anwenden,
    //         Puffer leeren, Transaktion beenden.
    //         Phase 72: TX_COMMIT in WAL schreiben; WAL wird erst nach persist gelöscht.
    //         Persistierung (save) + deleteWal() muss der Aufrufer danach selbst machen.
    void applyAndCommit() {
        if (!inTransaction_)
            throw std::runtime_error("Keine aktive Transaktion.");
        for (const auto& op : txBuffer_)
            applyOp(op);
        txBuffer_.clear();
        savepointStack_.clear();         // Phase 64
        g_lockManager.releaseRowLocks(std::this_thread::get_id());  // Phase 65
        // Phase 71: MVCC commit — mark tx as committed, then clear txId
        uint64_t commitId = mvccTxId_;
        if (mvccTxId_ != 0) {
            txManager_.commitTx(mvccTxId_);
            mvccTxId_ = 0;
        }
        inTransaction_ = false;
        // Phase 72: Write TX_COMMIT to WAL so recovery knows this tx was committed.
        // WAL is deleted by caller (dispatch) AFTER successful persist.
        if (!walPath_.empty() && commitId != 0) {
            std::ofstream wal(walPath_, std::ios::app);
            if (wal) wal << "TX_COMMIT:" << commitId << "\n";
        }
        // Phase 85: track committed transaction for checkpointing
        checkpointMgr_.onCommit();
    }

    // ROLLBACK: Puffer verwerfen, Transaktion abbrechen, WAL löschen.
    //           Die Tabellen bleiben unverändert.
    void rollbackTransaction() {
        if (!inTransaction_)
            throw std::runtime_error("Keine aktive Transaktion.");
        txBuffer_.clear();
        savepointStack_.clear();         // Phase 64
        g_lockManager.releaseRowLocks(std::this_thread::get_id());  // Phase 65
        // Phase 71: MVCC rollback — remove tx from active set (not committed)
        uint64_t rollbackId = mvccTxId_;
        if (mvccTxId_ != 0) {
            txManager_.rollbackTx(mvccTxId_);
            mvccTxId_ = 0;
        }
        inTransaction_ = false;
        // Phase 72: Write TX_ROLLBACK, then clean up WAL
        if (!walPath_.empty() && rollbackId != 0) {
            std::ofstream wal(walPath_, std::ios::app);
            if (wal) wal << "TX_ROLLBACK:" << rollbackId << "\n";
        }
        deleteWal();
    }

    // ── Phase 64: SAVEPOINT ──────────────────────────────────────

    void createSavepoint(const std::string& name) {
        if (!inTransaction_)
            throw std::runtime_error("SAVEPOINT erfordert eine aktive Transaktion (BEGIN).");
        savepointStack_.push_back({name, txBuffer_.size()});
    }

    void rollbackToSavepoint(const std::string& name) {
        if (!inTransaction_)
            throw std::runtime_error("Keine aktive Transaktion.");
        for (int i = static_cast<int>(savepointStack_.size()) - 1; i >= 0; --i) {
            if (savepointStack_[static_cast<size_t>(i)].name == name) {
                txBuffer_.resize(savepointStack_[static_cast<size_t>(i)].txSize);
                savepointStack_.resize(static_cast<size_t>(i));
                return;
            }
        }
        throw std::runtime_error("SAVEPOINT '" + name + "' nicht gefunden.");
    }

    void releaseSavepoint(const std::string& name) {
        if (!inTransaction_)
            throw std::runtime_error("Keine aktive Transaktion.");
        for (int i = static_cast<int>(savepointStack_.size()) - 1; i >= 0; --i) {
            if (savepointStack_[static_cast<size_t>(i)].name == name) {
                savepointStack_.resize(static_cast<size_t>(i));
                return;
            }
        }
        throw std::runtime_error("SAVEPOINT '" + name + "' nicht gefunden.");
    }

    // ── Phase 65: LOCK TABLE / UNLOCK TABLES / SELECT FOR UPDATE ────

    void lockTable(const std::string& tblRaw, const std::string& lockTypeStr) {
        auto tbl = resolveTableName(tblRaw);
        LockType lt = (lockTypeStr == "READ") ? LockType::READ : LockType::WRITE;
        if (!g_lockManager.acquireTableLock(tbl, lt))
            throw std::runtime_error("LOCK TABLE Timeout — Tabelle '" + tbl + "' ist durch einen anderen Thread gesperrt.");
    }

    void unlockTables() {
        g_lockManager.releaseTableLocks(std::this_thread::get_id());
    }

    std::vector<std::string> showLockInfo() const {
        return g_lockManager.showLocks();
    }

    // Acquire row-level WRITE locks on all rows returned by a SELECT FOR UPDATE.
    // Uses the first column of each result row as the row key.
    void acquireForUpdateLocks(const std::string& tblRaw, const Table& result) {
        auto tbl = resolveTableName(tblRaw);
        for (const auto& row : result.rows()) {
            if (!row.values.empty()) {
                if (!g_lockManager.acquireRowLock(tbl, row.values[0]))
                    throw std::runtime_error(
                        "SELECT FOR UPDATE Timeout — Zeile '" + row.values[0] +
                        "' in '" + tbl + "' ist bereits gesperrt.");
            }
        }
    }

    // WAL-Datei löschen (wird auch von main.cpp beim Start aufgerufen)
    void deleteWal() {
        if (!walPath_.empty())
            std::remove(walPath_.c_str());
    }

    // ── Metadaten ─────────────────────────────────────────────

    std::vector<IndexInfo> getIndexes(const std::string& tbl) const {
        return getTable(resolveTableName(tbl)).getIndexes();
    }

    std::vector<std::string> getAllTableNames() const {
        // Phase 51: return tables in current schema (strip prefix) + bare-name tables
        std::vector<std::string> names;
        std::string prefix = currentSchema_ + ".";
        for (const auto& [n, _] : tables_) {
            if (n.size() > prefix.size() && n.substr(0, prefix.size()) == prefix)
                names.push_back(n.substr(prefix.size()));
            else if (n.find('.') == std::string::npos)
                names.push_back(n);  // backward compat: bare names
        }
        return names;
    }

    // Returns full internal names (schema.table) for all tables
    std::vector<std::string> getAllTableNamesInternal() const {
        std::vector<std::string> names;
        for (const auto& [n, _] : tables_) names.push_back(n);
        return names;
    }

    // ── Phase 49: Full-Text Search ────────────────────────────────

    static std::vector<std::string> tokenizeText(const std::string& text) {
        static const std::set<std::string> stopwords = {
            "der", "die", "das", "und", "oder", "in", "ist", "ein", "eine",
            "the", "a", "an", "of", "to", "and", "or", "is", "for", "with",
            "mit", "fuer", "fur", "von", "auf", "im", "zu"
        };
        std::vector<std::string> tokens;
        std::string cur;
        for (char c : text) {
            if (std::isalpha((unsigned char)c) || std::isdigit((unsigned char)c) || c == '+') {
                cur += std::tolower((unsigned char)c);
            } else {
                if (!cur.empty()) {
                    if (!stopwords.count(cur)) tokens.push_back(cur);
                    cur.clear();
                }
            }
        }
        if (!cur.empty() && !stopwords.count(cur)) tokens.push_back(cur);
        return tokens;
    }

    void buildFulltextIndex(FullTextIndex& idx, const Table& table) {
        idx.invertedIndex.clear();
        idx.forwardIndex.clear();
        idx.rowWordCount.clear();

        const auto& cols = table.columns();
        const auto& rows = table.rows();

        for (size_t rowI = 0; rowI < rows.size(); ++rowI) {
            std::string fullText;
            for (const auto& colName : idx.cols) {
                for (size_t c = 0; c < cols.size(); ++c) {
                    if (cols[c].name == colName && c < rows[rowI].values.size()) {
                        fullText += " " + rows[rowI].values[c];
                        break;
                    }
                }
            }

            auto words = tokenizeText(fullText);
            idx.rowWordCount[rowI] = (int)words.size();

            for (const auto& w : words) {
                idx.invertedIndex[w].push_back(rowI);
                idx.forwardIndex[rowI][w]++;
            }
        }

        // Deduplicate invertedIndex entries
        for (auto& [w, idxList] : idx.invertedIndex) {
            std::sort(idxList.begin(), idxList.end());
            idxList.erase(std::unique(idxList.begin(), idxList.end()), idxList.end());
        }
    }

    void createFulltextIndex(const std::string& idxName, const std::string& tableNameRaw,
                              const std::vector<std::string>& cols) {
        auto tableName = resolveTableName(tableNameRaw);
        auto it = tables_.find(tableName);
        if (it == tables_.end())
            throw std::runtime_error("Tabelle '" + tableName + "' nicht gefunden");

        FullTextIndex idx;
        idx.name = idxName;
        idx.tableName = tableName;
        idx.cols = cols;

        buildFulltextIndex(idx, it->second);
        fulltextIndices_[idxName] = idx;
    }

    void dropFulltextIndex(const std::string& idxName) {
        fulltextIndices_.erase(idxName);
    }

    // Returns {rowIndex, score} sorted by score descending
    std::vector<std::pair<size_t, double>> searchFulltext(
            const std::string& tableNameRaw,
            const std::vector<std::string>& searchCols,
            const std::string& query) const {
        auto tableName = resolveTableName(tableNameRaw);

        // Find the fulltext index for this table+cols combination
        FullTextIndex const* idx = nullptr;
        for (const auto& [n, fi] : fulltextIndices_) {
            if (fi.tableName == tableName) {
                bool ok = true;
                for (const auto& sc : searchCols) {
                    bool found = false;
                    for (const auto& ic : fi.cols) if (ic == sc) { found = true; break; }
                    if (!found) { ok = false; break; }
                }
                if (ok) { idx = &fi; break; }
            }
        }

        auto queryWords = tokenizeText(query);
        if (queryWords.empty()) return {};

        std::map<size_t, double> scores;

        if (idx) {
            for (const auto& qw : queryWords) {
                auto it = idx->invertedIndex.find(qw);
                if (it == idx->invertedIndex.end()) continue;
                for (size_t rowI : it->second) {
                    int termCount = 0;
                    auto fi2 = idx->forwardIndex.find(rowI);
                    if (fi2 != idx->forwardIndex.end()) {
                        auto wi = fi2->second.find(qw);
                        if (wi != fi2->second.end()) termCount = wi->second;
                    }
                    int totalWords = 1;
                    auto wc = idx->rowWordCount.find(rowI);
                    if (wc != idx->rowWordCount.end()) totalWords = std::max(1, wc->second);
                    scores[rowI] += (double)termCount / totalWords;
                }
            }
        } else {
            auto tblIt = tables_.find(tableName);
            if (tblIt == tables_.end()) return {};
            const auto& table = tblIt->second;
            const auto& cols = table.columns();
            const auto& rows = table.rows();

            for (size_t rowI = 0; rowI < rows.size(); ++rowI) {
                std::string fullText;
                for (const auto& colName : searchCols) {
                    for (size_t c = 0; c < cols.size(); ++c) {
                        if (cols[c].name == colName && c < rows[rowI].values.size()) {
                            fullText += " " + rows[rowI].values[c];
                            break;
                        }
                    }
                }
                auto words = tokenizeText(fullText);
                int totalWords = std::max(1, (int)words.size());
                std::map<std::string,int> wc;
                for (const auto& w : words) wc[w]++;
                for (const auto& qw : queryWords) {
                    auto it = wc.find(qw);
                    if (it != wc.end()) scores[rowI] += (double)it->second / totalWords;
                }
            }
        }

        std::vector<std::pair<size_t, double>> result;
        for (const auto& [rowI, sc] : scores)
            if (sc > 0.0) result.push_back({rowI, sc});
        std::sort(result.begin(), result.end(),
                  [](const auto& a, const auto& b){ return a.second > b.second; });
        return result;
    }

    // Returns fulltext indexes for a given table (for SHOW INDEXES)
    std::vector<IndexInfo> getFulltextIndexes(const std::string& tableNameRaw) const {
        auto tableName = resolveTableName(tableNameRaw);
        std::vector<IndexInfo> result;
        for (const auto& [n, fi] : fulltextIndices_) {
            if (fi.tableName == tableName) {
                std::string colList;
                for (size_t i = 0; i < fi.cols.size(); ++i) {
                    if (i > 0) colList += ", ";
                    colList += fi.cols[i];
                }
                result.push_back({fi.name, colList, "FULLTEXT"});
            }
        }
        return result;
    }

    bool tableExists(const std::string& n) const {
        return tables_.count(resolveTableName(n)) > 0;
    }

    // ── Phase 31/32: CASE/Func — Projektion mit Ausdrücken ──────
    Table projectWithItems(const Table& src,
                           const std::vector<SelectItem>& items) const {
        // Phase 90: make extension manager available to static evaluateFunc
        ExtMgrGuard extGuard(const_cast<ExtensionManager*>(&extensionMgr_));
        // Ergebnis-Schema aufbauen
        std::vector<Column> newCols;
        for (const auto& item : items) {
            if (item.isUnnest) {
                // Phase 88: UNNEST — use alias if given, else column name
                std::string cname = item.alias.empty() ? item.unnestCol : item.alias;
                newCols.emplace_back(cname, "TEXT");
            } else if (item.isMatchAgainst) {
                // Phase 49: MATCH AGAINST score column
                std::string cname = item.alias.empty() ? "score" : item.alias;
                newCols.emplace_back(cname, "TEXT");
            } else if (item.isFuncExpr) {
                // Phase 32: String-Funktionen
                std::string cname;
                if (!item.alias.empty()) {
                    cname = item.alias;
                } else {
                    cname = item.funcName + "(";
                    for (size_t k = 0; k < item.funcArgs.size(); ++k) {
                        if (k > 0) cname += ",";
                        cname += item.funcArgs[k];
                    }
                    cname += ")";
                }
                newCols.emplace_back(cname, "TEXT");
            } else if (item.isCaseExpr) {
                std::string cname = item.alias.empty() ? "case" : item.alias;
                newCols.emplace_back(cname, "TEXT");
            } else if (item.isScalarSubquery) {
                std::string cname = item.alias.empty() ? "subquery" : item.alias;
                newCols.emplace_back(cname, "TEXT");
            } else {
                int ci = findColIdx(src, item.colName);
                if (ci < 0)
                    throw std::runtime_error(
                        "SELECT: Spalte '" + item.colName + "' nicht gefunden.");
                std::string cname = item.alias.empty()
                    ? src.columns()[static_cast<size_t>(ci)].name
                    : item.alias;
                newCols.emplace_back(cname, src.columns()[static_cast<size_t>(ci)].type);
            }
        }

        Table result("", newCols);
        for (const auto& row : src.rows()) {
            std::vector<std::string> vals;
            vals.reserve(items.size());
            for (const auto& item : items) {
                if (item.isUnnest) {
                    // Phase 88: pass array value through; expansion done in dispatch
                    int ci = findColIdx(src, item.unnestCol);
                    vals.push_back(ci >= 0 && static_cast<size_t>(ci) < row.values.size()
                                   ? row.values[static_cast<size_t>(ci)] : "");
                } else if (item.isMatchAgainst) {
                    // Phase 49: Compute TF score for this row
                    std::string text;
                    for (const auto& col : item.matchCols) {
                        int ci2 = findColIdx(src, col);
                        if (ci2 >= 0 && static_cast<size_t>(ci2) < row.values.size())
                            text += " " + row.values[static_cast<size_t>(ci2)];
                    }
                    auto docWords = tokenizeText(text);
                    int totalWords = std::max(1, (int)docWords.size());
                    std::map<std::string,int> wc;
                    for (const auto& w : docWords) wc[w]++;
                    double score = 0.0;
                    auto qWords = tokenizeText(item.againstQuery);
                    for (const auto& qw : qWords) {
                        auto it2 = wc.find(qw);
                        if (it2 != wc.end()) score += (double)it2->second / totalWords;
                    }
                    std::ostringstream oss;
                    oss << std::fixed << std::setprecision(4) << score;
                    vals.push_back(oss.str());
                } else if (item.isFuncExpr) {
                    vals.push_back(evaluateFunc(item.funcName, item.funcArgs, src, row));
                } else if (item.isCaseExpr) {
                    vals.push_back(evalCase(item, src, row));
                } else if (item.isScalarSubquery) {
                    vals.push_back(evalScalarSub(item.scalarSub, src.columns(), row));
                } else {
                    int ci = findColIdx(src, item.colName);
                    vals.push_back(ci >= 0 && static_cast<size_t>(ci) < row.values.size()
                                   ? row.values[static_cast<size_t>(ci)] : "");
                }
            }
            result.insert(Row(vals));
        }
        return result;
    }

    // ── Phase 42: Project with window functions ──────────────────
    // If selectItems contain window functions, applies them and then
    // projects the result. Returns a new Table with all columns.
    Table projectWithWindowItems(const Table& src,
                                  const std::vector<SelectItem>& items) const {
        // Phase 90: make extension manager available to static evaluateFunc
        ExtMgrGuard extGuard(const_cast<ExtensionManager*>(&extensionMgr_));
        // Separate window items from non-window items
        std::vector<SelectItem> windowItems, normalItems;
        for (const auto& item : items) {
            if (item.isWindowFunc) windowItems.push_back(item);
            else normalItems.push_back(item);
        }

        if (windowItems.empty())
            return projectWithItems(src, items);

        // Copy rows mutably so we can append window values
        std::vector<Row> rows(src.rows().begin(), src.rows().end());

        // Build extended column names (src columns + window aliases)
        std::vector<std::string> extColNames;
        for (const auto& c : src.columns()) extColNames.push_back(c.name);

        // Apply window functions (appends to each row, extends extColNames)
        applyWindowFunctions(rows, windowItems, src.columns(), extColNames);

        // Build extended columns vector for result table schema
        std::vector<Column> extCols = src.columns();
        for (size_t i = src.columns().size(); i < extColNames.size(); ++i)
            extCols.emplace_back(extColNames[i], "TEXT");

        // Build a temporary extended table
        Table extTable("", extCols);
        for (auto& r : rows) extTable.insert(r);

        // Now project the requested items from the extended table
        // For each item in the original items list:
        //   - if isWindowFunc: look up by alias in extTable columns
        //   - otherwise: use normal projectWithItems logic
        std::vector<Column> resultCols;
        for (const auto& item : items) {
            if (item.isWindowFunc) {
                std::string cname = item.alias.empty() ? item.windowFunc : item.alias;
                resultCols.emplace_back(cname, "TEXT");
            } else if (item.isFuncExpr) {
                std::string cname;
                if (!item.alias.empty()) {
                    cname = item.alias;
                } else {
                    cname = item.funcName + "(";
                    for (size_t k = 0; k < item.funcArgs.size(); ++k) {
                        if (k > 0) cname += ",";
                        cname += item.funcArgs[k];
                    }
                    cname += ")";
                }
                resultCols.emplace_back(cname, "TEXT");
            } else if (item.isCaseExpr) {
                std::string cname = item.alias.empty() ? "case" : item.alias;
                resultCols.emplace_back(cname, "TEXT");
            } else if (item.isScalarSubquery) {
                std::string cname = item.alias.empty() ? "subquery" : item.alias;
                resultCols.emplace_back(cname, "TEXT");
            } else if (item.colName == "*") {
                // Wildcard: include all src columns
                for (const auto& c : src.columns())
                    resultCols.emplace_back(item.alias.empty() ? c.name : item.alias, c.type);
            } else {
                int ci = findColIdx(extTable, item.colName);
                if (ci < 0)
                    throw std::runtime_error(
                        "SELECT: Spalte '" + item.colName + "' nicht gefunden.");
                std::string cname = item.alias.empty()
                    ? extTable.columns()[static_cast<size_t>(ci)].name
                    : item.alias;
                resultCols.emplace_back(cname, extTable.columns()[static_cast<size_t>(ci)].type);
            }
        }

        Table result("", resultCols);
        for (const auto& row : extTable.rows()) {
            std::vector<std::string> vals;
            vals.reserve(items.size());
            for (const auto& item : items) {
                if (item.isWindowFunc) {
                    std::string cname = item.alias.empty() ? item.windowFunc : item.alias;
                    int ci = findColIdx(extTable, cname);
                    vals.push_back(ci >= 0 && static_cast<size_t>(ci) < row.values.size()
                                   ? row.values[static_cast<size_t>(ci)] : "");
                } else if (item.isFuncExpr) {
                    vals.push_back(evaluateFunc(item.funcName, item.funcArgs, extTable, row));
                } else if (item.isCaseExpr) {
                    vals.push_back(evalCase(item, extTable, row));
                } else if (item.isScalarSubquery) {
                    vals.push_back(evalScalarSub(item.scalarSub, extTable.columns(), row));
                } else if (item.colName == "*") {
                    // Wildcard: include all src columns (not the appended window cols)
                    for (size_t ci = 0; ci < src.columns().size(); ++ci)
                        vals.push_back(ci < row.values.size() ? row.values[ci] : "");
                } else {
                    int ci = findColIdx(extTable, item.colName);
                    vals.push_back(ci >= 0 && static_cast<size_t>(ci) < row.values.size()
                                   ? row.values[static_cast<size_t>(ci)] : "");
                }
            }
            result.insert(Row(vals));
        }
        return result;
    }

    // ── Phase 30: Mengenoperationen ───────────────────────────
    // op: "UNION", "UNION ALL", "INTERSECT", "EXCEPT"
    // Spaltenbreite beider Seiten muss übereinstimmen.
    // Spaltenname kommt aus linker Tabelle.
    Table executeSetOp(const Table& left,
                       const std::string& op,
                       const Table& right) const {
        if (left.columns().size() != right.columns().size())
            throw std::runtime_error(
                "Mengenoperation: Spaltenanzahl muss übereinstimmen ("
                + std::to_string(left.columns().size()) + " vs "
                + std::to_string(right.columns().size()) + ")");

        // Ergebnis-Schema = Spalten der linken Seite
        Table result("", left.columns());

        if (op == "UNION ALL") {
            // Alle linken + alle rechten Zeilen (Duplikate erlaubt)
            for (const auto& r : left.rows())  result.insert(r);
            for (const auto& r : right.rows()) result.insert(r);

        } else if (op == "UNION") {
            // Vereinigung ohne Duplikate
            std::vector<std::vector<std::string>> seen;
            auto isDup = [&](const Row& r) {
                for (const auto& s : seen) if (s == r.values) return true;
                return false;
            };
            for (const auto& r : left.rows())
                if (!isDup(r)) { result.insert(r); seen.push_back(r.values); }
            for (const auto& r : right.rows())
                if (!isDup(r)) { result.insert(r); seen.push_back(r.values); }

        } else if (op == "INTERSECT") {
            // Nur Zeilen, die in beiden vorkommen (ohne Duplikate)
            std::vector<std::vector<std::string>> seen;
            auto isDup = [&](const Row& r) {
                for (const auto& s : seen) if (s == r.values) return true;
                return false;
            };
            for (const auto& lr : left.rows()) {
                if (isDup(lr)) continue;
                for (const auto& rr : right.rows()) {
                    if (lr.values == rr.values) {
                        result.insert(lr);
                        seen.push_back(lr.values);
                        break;
                    }
                }
            }

        } else if (op == "EXCEPT") {
            // Zeilen aus links, die NICHT in rechts vorkommen (ohne Duplikate)
            std::vector<std::vector<std::string>> seen;
            auto isDup = [&](const Row& r) {
                for (const auto& s : seen) if (s == r.values) return true;
                return false;
            };
            for (const auto& lr : left.rows()) {
                if (isDup(lr)) continue;
                bool inRight = false;
                for (const auto& rr : right.rows())
                    if (lr.values == rr.values) { inRight = true; break; }
                if (!inRight) { result.insert(lr); seen.push_back(lr.values); }
            }
        } else {
            throw std::runtime_error("Unbekannte Mengenoperation: " + op);
        }

        return result;
    }

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

    // ── Phase 42: Window Functions ───────────────────────────────
    // Applies window functions to rows in-place.
    // windowItems: SelectItems with isWindowFunc==true.
    // The computed value is appended to each row's values vector.
    // colNames: extended with the alias (or function name) for each window item.
    static void applyWindowFunctions(
            std::vector<Row>& rows,
            const std::vector<SelectItem>& windowItems,
            const std::vector<Column>& columns,
            std::vector<std::string>& colNames) {

        // Helper: get value of a named column from a row
        auto getColValue = [&](const Row& row, const std::string& colName) -> std::string {
            for (size_t i = 0; i < columns.size(); ++i)
                if (columns[i].name == colName && i < row.values.size())
                    return row.values[i];
            // Also check colNames (already-appended window columns)
            for (size_t i = 0; i < colNames.size(); ++i)
                if (colNames[i] == colName && (columns.size() + i) < row.values.size())
                    return row.values[columns.size() + i];
            return "";
        };

        // Numeric-aware comparator for sorting
        auto numCmp = [](const std::string& a, const std::string& b, bool desc) -> bool {
            try {
                size_t ea = 0, eb = 0;
                double da = std::stod(a, &ea);
                double db = std::stod(b, &eb);
                if (ea == a.size() && eb == b.size())
                    return desc ? da > db : da < db;
            } catch (...) {}
            return desc ? a > b : a < b;
        };

        for (const auto& item : windowItems) {
            const std::string& wfunc = item.windowFunc;
            std::string alias = item.alias.empty() ? wfunc : item.alias;

            // Group row indices by partition key
            std::map<std::string, std::vector<size_t>> partitions;
            std::vector<std::string> partOrder;  // insertion order

            for (size_t i = 0; i < rows.size(); ++i) {
                std::string key = "";
                if (!item.windowPartitionBy.empty())
                    key = getColValue(rows[i], item.windowPartitionBy);
                if (!partitions.count(key)) partOrder.push_back(key);
                partitions[key].push_back(i);
            }

            // For each row, we'll store the window value
            std::vector<std::string> windowVals(rows.size(), "");

            for (const auto& key : partOrder) {
                auto& idxList = partitions[key];

                if (wfunc == "ROW_NUMBER" || wfunc == "RANK" || wfunc == "DENSE_RANK") {
                    // Sort partition by ORDER BY column
                    std::vector<size_t> sorted = idxList;
                    if (!item.windowOrderBy.empty()) {
                        bool desc = item.windowOrderDesc;
                        std::stable_sort(sorted.begin(), sorted.end(),
                            [&](size_t a, size_t b) {
                                return numCmp(
                                    getColValue(rows[a], item.windowOrderBy),
                                    getColValue(rows[b], item.windowOrderBy),
                                    desc);
                            });
                    }

                    if (wfunc == "ROW_NUMBER") {
                        for (size_t r = 0; r < sorted.size(); ++r)
                            windowVals[sorted[r]] = std::to_string(r + 1);
                    } else if (wfunc == "RANK") {
                        int rank = 1;
                        for (size_t r = 0; r < sorted.size(); ) {
                            // Find group of equal values
                            size_t j = r + 1;
                            while (j < sorted.size() &&
                                   !item.windowOrderBy.empty() &&
                                   getColValue(rows[sorted[j]], item.windowOrderBy) ==
                                   getColValue(rows[sorted[r]], item.windowOrderBy))
                                ++j;
                            for (size_t k = r; k < j; ++k)
                                windowVals[sorted[k]] = std::to_string(rank);
                            rank += static_cast<int>(j - r);
                            r = j;
                        }
                    } else {  // DENSE_RANK
                        int rank = 1;
                        for (size_t r = 0; r < sorted.size(); ) {
                            size_t j = r + 1;
                            while (j < sorted.size() &&
                                   !item.windowOrderBy.empty() &&
                                   getColValue(rows[sorted[j]], item.windowOrderBy) ==
                                   getColValue(rows[sorted[r]], item.windowOrderBy))
                                ++j;
                            for (size_t k = r; k < j; ++k)
                                windowVals[sorted[k]] = std::to_string(rank);
                            ++rank;
                            r = j;
                        }
                    }

                } else {
                    // Aggregate over entire partition: SUM, AVG, COUNT, MIN, MAX
                    if (wfunc == "COUNT") {
                        std::string cnt = std::to_string(idxList.size());
                        for (size_t idx : idxList)
                            windowVals[idx] = cnt;
                    } else {
                        // Get column index for the aggregate argument
                        std::vector<double> nums;
                        for (size_t idx : idxList) {
                            std::string v = getColValue(rows[idx], item.windowFuncArg);
                            try { nums.push_back(std::stod(v)); } catch (...) {}
                        }
                        std::string aggResult = "NULL";
                        if (!nums.empty()) {
                            if (wfunc == "SUM") {
                                double s = 0; for (double v : nums) s += v;
                                aggResult = formatNum(s);
                            } else if (wfunc == "AVG") {
                                double s = 0; for (double v : nums) s += v;
                                aggResult = formatNum(s / static_cast<double>(nums.size()));
                            } else if (wfunc == "MIN") {
                                aggResult = formatNum(*std::min_element(nums.begin(), nums.end()));
                            } else if (wfunc == "MAX") {
                                aggResult = formatNum(*std::max_element(nums.begin(), nums.end()));
                            }
                        }
                        for (size_t idx : idxList)
                            windowVals[idx] = aggResult;
                    }
                }
            }

            // Append window values to each row
            for (size_t i = 0; i < rows.size(); ++i)
                rows[i].values.push_back(windowVals[i]);
            colNames.push_back(alias);
        }
    }

    // ── Phase 43: Trigger Management ─────────────────────────────

    void createTrigger(const TriggerDef& def) {
        triggers_[def.name] = def;
    }

    void dropTrigger(const std::string& name) {
        if (!triggers_.erase(name))
            throw std::runtime_error("Trigger '" + name + "' nicht gefunden.");
    }

    std::vector<TriggerDef> showTriggers(const std::string& tableName = "") const {
        std::vector<TriggerDef> result;
        for (const auto& [n, t] : triggers_) {
            if (tableName.empty() || t.tableName == tableName)
                result.push_back(t);
        }
        return result;
    }

    const std::map<std::string, TriggerDef>& getAllTriggers() const {
        return triggers_;
    }

    // Phase 93: Fire all AFTER STATEMENT-level triggers for a DML event.
    void fireStatementTriggers(const std::string& event, const std::string& tableName) {
        for (auto& [trgName, trg] : triggers_) {
            if (trgUpper(trg.timing)       != "AFTER")          continue;
            if (trgUpper(trg.event)        != trgUpper(event))  continue;
            if (trg.tableName              != tableName)         continue;
            if (trgUpper(trg.granularity)  != "STATEMENT")      continue;
            // Execute body without row context (empty OLD/NEW)
            std::vector<std::string> emptyRow;
            std::string signalMsg;
            executeTriggerBody(trg.body, emptyRow, emptyRow, {}, signalMsg, false);
        }
    }

    // ── Phase 44: Stored Procedure Management ────────────────────

    void createProcedure(const ProcedureDef& def) {
        procedures_[def.name] = def;
    }

    void dropProcedure(const std::string& name) {
        if (!procedures_.erase(name))
            throw std::runtime_error("Procedure '" + name + "' nicht gefunden.");
    }

    std::vector<ProcedureDef> showProcedures() const {
        std::vector<ProcedureDef> result;
        for (const auto& [n, p] : procedures_)
            result.push_back(p);
        return result;
    }

    const std::map<std::string, ProcedureDef>& getAllProcedures() const {
        return procedures_;
    }

    // ── Phase 44: Arithmetic Expression Evaluation for SET values ──
    // Evaluates a simple expression like "col + num", "col - num",
    // "col * num", "col / num" given a row context.
    // Returns the string result, or the original expr if not an arithmetic expr.
    static std::string evalSetExpr(const std::string& expr,
                                    const std::vector<Column>& cols,
                                    const Row& row) {
        // Tokenize on spaces
        std::vector<std::string> toks;
        {
            std::istringstream iss(expr);
            std::string t;
            while (iss >> t) toks.push_back(t);
        }
        // Pattern: col op num  (3 tokens)
        if (toks.size() == 3) {
            const std::string& op = toks[1];
            if (op == "+" || op == "-" || op == "*" || op == "/") {
                // Resolve lhs (column or numeric literal)
                std::string lhsStr = toks[0];
                for (size_t i = 0; i < cols.size(); ++i)
                    if (cols[i].name == toks[0] && i < row.values.size()) {
                        lhsStr = row.values[i]; break;
                    }
                // Resolve rhs (column or numeric literal)
                std::string rhsStr = toks[2];
                for (size_t i = 0; i < cols.size(); ++i)
                    if (cols[i].name == toks[2] && i < row.values.size()) {
                        rhsStr = row.values[i]; break;
                    }
                try {
                    double lv = std::stod(lhsStr);
                    double rv = std::stod(rhsStr);
                    double res = 0;
                    if (op == "+") res = lv + rv;
                    else if (op == "-") res = lv - rv;
                    else if (op == "*") res = lv * rv;
                    else if (op == "/" && rv != 0) res = lv / rv;
                    else return expr;
                    // Return as integer if possible
                    if (res == std::floor(res))
                        return std::to_string(static_cast<long long>(res));
                    return std::to_string(res);
                } catch (...) {}
            }
        }
        // Single token: column lookup or literal
        if (toks.size() == 1) {
            for (size_t i = 0; i < cols.size(); ++i)
                if (cols[i].name == toks[0] && i < row.values.size())
                    return row.values[i];
        }
        return expr;
    }

    // Returns body with parameter substitution applied (whole-word matching)
    std::string getProcedureBody(const std::string& name,
                                  const std::vector<std::string>& args) const {
        auto it = procedures_.find(name);
        if (it == procedures_.end())
            throw std::runtime_error("Procedure '" + name + "' nicht gefunden");
        const auto& proc = it->second;
        std::string body = proc.body;
        for (size_t i = 0; i < proc.params.size() && i < args.size(); ++i) {
            const std::string& paramName = proc.params[i].first;
            const std::string& argVal    = args[i];
            std::string result;
            size_t pos = 0;
            while (pos < body.size()) {
                size_t found = body.find(paramName, pos);
                if (found == std::string::npos) { result += body.substr(pos); break; }
                bool leftOk  = (found == 0 ||
                    (!std::isalnum(static_cast<unsigned char>(body[found-1])) &&
                      body[found-1] != '_'));
                bool rightOk = (found + paramName.size() >= body.size() ||
                    (!std::isalnum(static_cast<unsigned char>(
                        body[found + paramName.size()])) &&
                      body[found + paramName.size()] != '_'));
                if (leftOk && rightOk) {
                    result += body.substr(pos, found - pos) + argVal;
                    pos = found + paramName.size();
                } else {
                    result += body.substr(pos, found - pos + 1);
                    pos = found + 1;
                }
            }
            body = result;
        }
        return body;
    }

    // ── Phase 41: WITH / CTE — temporäre Tabellen ────────────────

    // Registriert eine Tabelle unter name als temporäre CTE-Tabelle.
    void registerTempTable(const std::string& name, Table tbl) {
        tempTableNames_.insert(name);
        // Move into tables_ under the CTE name (clone with new name)
        Table named(name, tbl.columns());
        for (const auto& row : tbl.rows()) named.insert(row);
        tables_[name] = std::move(named);
    }

    // Entfernt alle registrierten CTE-Tabellen aus tables_.
    void cleanupTempTables() {
        for (const auto& n : tempTableNames_) tables_.erase(n);
        tempTableNames_.clear();
    }

    // Entfernt eine einzelne Temp-Tabelle (Phase 89: FDW cleanup)
    void dropTempTable(const std::string& name) {
        tables_.erase(name);
        tempTableNames_.erase(name);
    }

    // ── Phase 45: Prepared Statements ────────────────────────────

    void prepareStmt(const std::string& name, const std::string& sql) {
        PreparedStmt stmt;
        stmt.name = name;
        stmt.sql = sql;
        stmt.paramCount = (int)std::count(sql.begin(), sql.end(), '?');
        preparedStmts_[name] = stmt;
    }

    void deallocateStmt(const std::string& name) {
        preparedStmts_.erase(name);
    }

    std::vector<PreparedStmt> showPrepared() const {
        std::vector<PreparedStmt> result;
        for (const auto& [n, s] : preparedStmts_)
            result.push_back(s);
        return result;
    }

    // ── Phase 46: User Management ────────────────────────────────

    void initUsers() {
        if (users_.find("root") == users_.end()) {
            UserDef root;
            root.name = "root";
            root.passwordHash = 0;
            users_["root"] = root;
        }
    }

    void createUser(const std::string& name, const std::string& password) {
        if (users_.count(name))
            throw std::runtime_error("User '" + name + "' existiert bereits");
        UserDef u;
        u.name = name;
        u.passwordHash = std::hash<std::string>{}(password);
        users_[name] = u;
        saveUsers();
    }

    void dropUser(const std::string& name) {
        if (name == "root")
            throw std::runtime_error("root kann nicht geloescht werden");
        users_.erase(name);
        saveUsers();
    }

    std::vector<UserDef> showUsers() const {
        std::vector<UserDef> result;
        for (const auto& [n, u] : users_) result.push_back(u);
        return result;
    }

    bool connectUser(const std::string& name, const std::string& password) {
        auto it = users_.find(name);
        if (it == users_.end()) return false;
        if (name == "root") {
            currentUser_ = "root";
            return true;
        }
        if (it->second.passwordHash != std::hash<std::string>{}(password)) return false;
        currentUser_ = name;
        return true;
    }

    void disconnectUser() {
        currentUser_ = "root";
    }

    const std::string& getCurrentUser() const { return currentUser_; }

    // ── Phase 78: Table Inheritance ──────────────────────────────
    bool tableHasParent(const std::string& tbl) const {
        return tableParent_.count(resolveTableName(tbl)) > 0;
    }
    std::string tableParentName(const std::string& tbl) const {
        auto it = tableParent_.find(resolveTableName(tbl));
        return it != tableParent_.end() ? it->second : "";
    }
    // Called by storage layer to restore inheritance maps without touching columns
    void registerInheritance(const std::string& child, const std::string& parent) {
        auto ck = resolveTableName(child);
        auto pk = resolveTableName(parent);
        tableParent_[ck] = pk;
        auto& vec = tableChildren_[pk];
        if (std::find(vec.begin(), vec.end(), ck) == vec.end())
            vec.push_back(ck);
    }

    void showInheritance() const {
        std::cout << "\n  Table Inheritance:\n";
        // Print roots that have children
        bool any = false;
        for (const auto& [key, children] : tableChildren_) {
            if (children.empty()) continue;
            any = true;
            std::cout << "  " << key << "\n";
            printInheritanceTree_(key, 1);
        }
        if (!any) std::cout << "  (keine Vererbung definiert)\n";
        std::cout << "\n";
    }

    // ── Phase 80: Column Store Engine ────────────────────────

    void createColumnTable(const std::string& name,
                           const std::vector<Column>& cols) {
        auto key = resolveTableName(name);
        if (columnTables_.count(key))
            throw std::runtime_error("Column Table '" + key + "' existiert bereits.");
        if (tables_.count(key))
            throw std::runtime_error("Tabelle '" + key + "' existiert bereits.");
        columnTables_.emplace(key, ColumnTable(key, cols));
    }

    bool isColumnTable(const std::string& name) const {
        return columnTables_.count(resolveTableName(name)) > 0;
    }

    ColumnTable& getColumnTable(const std::string& name) {
        auto it = columnTables_.find(resolveTableName(name));
        if (it == columnTables_.end())
            throw std::runtime_error("Column Table '" + name + "' nicht gefunden.");
        return it->second;
    }

    const ColumnTable& getColumnTableConst(const std::string& name) const {
        auto it = columnTables_.find(resolveTableName(name));
        if (it == columnTables_.end())
            throw std::runtime_error("Column Table '" + name + "' nicht gefunden.");
        return it->second;
    }

    void showStorageFormat() const {
        std::cout << "\n  Storage Format Report:\n";
        std::cout << "  +-----------------------+---------------+\n";
        std::cout << "  | Table                 | Format        |\n";
        std::cout << "  +-----------------------+---------------+\n";
        // Row-store tables (skip internal temp tables starting with __)
        for (const auto& [key, tbl] : tables_) {
            if (key.size() >= 2 && key[0] == '_' && key[1] == '_') continue;
            std::cout << "  | " << std::left << std::setw(21) << key
                      << "| ROW STORE     |\n";
        }
        // Column-store tables
        for (const auto& [key, tbl] : columnTables_) {
            std::cout << "  | " << std::left << std::setw(21) << key
                      << "| COLUMN STORE  |\n";
        }
        std::cout << "  +-----------------------+---------------+\n\n";
    }

    // ── Phase 84: Page-based I/O ─────────────────────────────

    bool isPagedTable(const std::string& name) const {
        return pagedTables_.count(resolveTableName(name)) > 0;
    }

    void createPagedTable(const std::string& name,
                          const std::vector<Column>& cols) {
        auto key = resolveTableName(name);
        if (pagedTables_.count(key))
            throw std::runtime_error("Paged Table '" + key + "' existiert bereits.");
        if (tables_.count(key))
            throw std::runtime_error("Tabelle '" + key + "' existiert bereits (ROW STORE).");
        if (columnTables_.count(key))
            throw std::runtime_error("Tabelle '" + key + "' existiert bereits (COLUMN STORE).");
        pagedTables_.emplace(key, PagedTable(key, cols, pagedManager_));
        savePagedTableSchemas_();
    }

    void insertIntoPagedTable(const std::string& name,
                              const std::vector<std::string>& vals) {
        auto key = resolveTableName(name);
        auto it = pagedTables_.find(key);
        if (it == pagedTables_.end())
            throw std::runtime_error("Paged Table '" + key + "' nicht gefunden.");
        it->second.insertRow(Row(vals));
    }

    Table selectFromPagedTable(const std::string& name) const {
        auto key = resolveTableName(name);
        auto it = pagedTables_.find(key);
        if (it == pagedTables_.end())
            throw std::runtime_error("Paged Table '" + key + "' nicht gefunden.");
        return it->second.toTable();
    }

    size_t countPagedTable(const std::string& name) const {
        auto key = resolveTableName(name);
        auto it = pagedTables_.find(key);
        if (it == pagedTables_.end()) return 0;
        return it->second.getRowCount();
    }

    void showPageStats() const {
        std::cout << "\n" << pagedManager_.showStats();
        if (pagedTables_.empty()) {
            std::cout << "  (keine Paged Tables vorhanden)\n\n";
            return;
        }
        for (const auto& [key, pt] : pagedTables_)
            std::cout << pt.pageStats();
        std::cout << "\n";
    }

    void flushPages() {
        pagedManager_.flushAll();
        std::cout << "  Alle Dirty Pages geschrieben.\n\n";
    }

    // ── Phase 81: Logical Replication ──────────────────────────
    void createPublication(const std::string& name,
                           const std::vector<std::string>& tables,
                           bool allTables) {
        if (publications_.count(name))
            throw std::runtime_error("Publication '" + name + "' existiert bereits.");
        PublicationDef pd;
        pd.name = name;
        pd.tables = tables;
        pd.allTables = allTables;
        publications_[name] = std::move(pd);
        savePublications_();
    }

    void dropPublication(const std::string& name) {
        if (!publications_.erase(name))
            throw std::runtime_error("Publication '" + name + "' nicht gefunden.");
        savePublications_();
    }

    void showPublications() const {
        std::cout << "\n  Publications:\n";
        if (publications_.empty()) { std::cout << "  (keine)\n\n"; return; }
        for (const auto& [n, pd] : publications_) {
            std::cout << "  " << n << ": ";
            if (pd.allTables) std::cout << "ALL TABLES";
            else {
                for (size_t i = 0; i < pd.tables.size(); ++i) {
                    if (i) std::cout << ", ";
                    std::cout << pd.tables[i];
                }
            }
            std::cout << "\n";
        }
        std::cout << "\n";
    }

    void createSubscription(const std::string& name,
                            const std::string& connection,
                            const std::string& publication) {
        if (subscriptions_.count(name))
            throw std::runtime_error("Subscription '" + name + "' existiert bereits.");
        SubscriptionDef sd;
        sd.name = name;
        sd.connection = connection;
        sd.publication = publication;
        sd.enabled = true;
        subscriptions_[name] = std::move(sd);
        saveSubscriptions_();
    }

    void dropSubscription(const std::string& name) {
        if (!subscriptions_.erase(name))
            throw std::runtime_error("Subscription '" + name + "' nicht gefunden.");
        saveSubscriptions_();
    }

    void showSubscriptions() const {
        std::cout << "\n  Subscriptions:\n";
        if (subscriptions_.empty()) { std::cout << "  (keine)\n\n"; return; }
        for (const auto& [n, sd] : subscriptions_) {
            std::cout << "  " << n << ": "
                      << sd.connection << " -> " << sd.publication
                      << " [" << (sd.enabled ? "ENABLED" : "DISABLED") << "]\n";
        }
        std::cout << "\n";
    }

    void alterSubscription(const std::string& name, bool enable) {
        auto it = subscriptions_.find(name);
        if (it == subscriptions_.end())
            throw std::runtime_error("Subscription '" + name + "' nicht gefunden.");
        it->second.enabled = enable;
        saveSubscriptions_();
    }

    void showLogicalLog() const {
        auto changes = logicalLog_.readChanges();
        std::cout << "\n  Logical Replication Log (" << changes.size() << " Eintraege):\n";
        if (changes.empty()) { std::cout << "  (leer)\n\n"; return; }
        for (const auto& ch : changes) {
            std::cout << "  [" << ch.op << "] " << ch.table << ": ";
            for (size_t i = 0; i < ch.cols.size(); ++i) {
                if (i) std::cout << ", ";
                std::cout << ch.cols[i] << "=" << ch.vals[i];
            }
            std::cout << "\n";
        }
        std::cout << "\n";
    }

    // ── Phase 89: Foreign Data Wrapper ───────────────────────

    void createServer(const std::string& name, const std::string& wrapperType) {
        servers_[name] = ServerDef{name, wrapperType};
    }

    void dropServer(const std::string& name) {
        servers_.erase(name);
    }

    void showServers() const {
        std::cout << "\n  Servers:\n";
        if (servers_.empty()) { std::cout << "  (keine)\n\n"; return; }
        for (const auto& [n, s] : servers_)
            std::cout << "  " << n << " (FOREIGN DATA WRAPPER: " << s.wrapperType << ")\n";
        std::cout << "\n";
    }

    void createForeignTable(ForeignTableDef def) {
        foreignTables_[def.name] = std::move(def);
    }

    void dropForeignTable(const std::string& name) {
        foreignTables_.erase(name);
    }

    void showForeignTables() const {
        std::cout << "\n  Foreign Tables:\n";
        if (foreignTables_.empty()) { std::cout << "  (keine)\n\n"; return; }
        for (const auto& [n, t] : foreignTables_) {
            std::cout << "  " << n << " SERVER " << t.serverName << " (";
            for (size_t i = 0; i < t.colNames.size(); ++i) {
                if (i) std::cout << ", ";
                std::cout << t.colNames[i] << " " << t.colTypes[i];
            }
            std::cout << ")\n";
        }
        std::cout << "\n";
    }

    bool isForeignTable(const std::string& name) const {
        return foreignTables_.count(name) > 0;
    }

    const ForeignTableDef& getForeignTable(const std::string& name) const {
        return foreignTables_.at(name);
    }

    const std::map<std::string, ServerDef>& getServers() const {
        return servers_;
    }

    // Execute a foreign table scan and return result as a Table.
    // String values are wrapped in single quotes to match the engine's storage
    // format (same as INSERT VALUES), enabling JOIN comparisons to work correctly.
    Table executeForeignScan(const std::string& tableName) {
        const auto& ftd = foreignTables_.at(tableName);
        auto srv_it = servers_.find(ftd.serverName);
        if (srv_it == servers_.end())
            throw std::runtime_error("Server '" + ftd.serverName + "' nicht gefunden.");
        const auto& srv = srv_it->second;

        std::vector<std::vector<std::string>> rows;
        if (srv.wrapperType == "csv") {
            CsvFdw fdw;
            rows = fdw.scan(ftd, ftd.colNames);
        } else if (srv.wrapperType == "http_json") {
            HttpFdw fdw;
            rows = fdw.scan(ftd, ftd.colNames);
        }

        std::vector<Column> cols;
        for (size_t i = 0; i < ftd.colNames.size(); ++i)
            cols.emplace_back(ftd.colNames[i],
                              i < ftd.colTypes.size() ? ftd.colTypes[i] : "TEXT");

        // Helper: determine if a column type is numeric (INT/FLOAT/DOUBLE/etc.)
        auto isNumericType = [](const std::string& type) -> bool {
            std::string up = type;
            for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return up == "INT"     || up == "INTEGER" || up == "BIGINT"  ||
                   up == "SMALLINT"|| up == "FLOAT"   || up == "DOUBLE"  ||
                   up == "DECIMAL" || up == "NUMERIC" || up == "REAL";
        };

        // Helper: check if a value is already quoted or is NULL
        auto needsQuoting = [](const std::string& val) -> bool {
            if (val == "NULL") return false;
            if (val.size() >= 2 && val.front() == '\'' && val.back() == '\'') return false;
            return true;
        };

        Table t(tableName, std::move(cols));
        for (const auto& r : rows) {
            std::vector<std::string> normalized;
            normalized.reserve(r.size());
            for (size_t i = 0; i < r.size(); ++i) {
                const std::string& colType = (i < ftd.colTypes.size()) ? ftd.colTypes[i] : "TEXT";
                const std::string& val = r[i];
                if (!isNumericType(colType) && needsQuoting(val)) {
                    // Wrap in single quotes (escape any internal quotes)
                    std::string quoted = "'";
                    for (char c : val) {
                        if (c == '\'') quoted += "''";
                        else           quoted += c;
                    }
                    quoted += "'";
                    normalized.push_back(std::move(quoted));
                } else {
                    normalized.push_back(val);
                }
            }
            t.insert(Row(normalized));
        }
        return t;
    }

    // ── Phase 77: Parallel Query ──────────────────────────────
    void setParallelThreshold(long long t) { if (t >= 1) parallelExec_.threshold = t; }
    void setMaxParallelWorkers(int n)      { if (n >= 1 && n <= 64) parallelExec_.numWorkers = n; }
    long long getParallelThreshold() const { return parallelExec_.threshold; }
    int  getMaxParallelWorkers() const     { return parallelExec_.numWorkers; }
    bool isParallelEnabled() const         { return parallelExec_.numWorkers > 1; }

    void showParallelStatus() const {
        std::cout << "\n  Parallel Query Execution:\n";
        std::cout << "    PARALLEL_THRESHOLD   = " << parallelExec_.threshold << " rows\n";
        std::cout << "    MAX_PARALLEL_WORKERS = " << parallelExec_.numWorkers << " threads\n";
        std::cout << "    Status               = "
                  << (parallelExec_.numWorkers > 1 ? "ENABLED" : "DISABLED") << "\n\n";
    }

    void grantPrivilege(const std::string& userName, const std::string& tableName,
                        const std::vector<std::string>& privs) {
        auto it = users_.find(userName);
        if (it == users_.end())
            throw std::runtime_error("User '" + userName + "' nicht gefunden");
        for (const auto& p : privs)
            it->second.grants[tableName].insert(toUpperStatic(p));
        saveUsers();
    }

    void revokePrivilege(const std::string& userName, const std::string& tableName,
                         const std::vector<std::string>& privs) {
        auto it = users_.find(userName);
        if (it == users_.end())
            throw std::runtime_error("User '" + userName + "' nicht gefunden");
        for (const auto& p : privs)
            it->second.grants[tableName].erase(toUpperStatic(p));
        saveUsers();
    }

    std::vector<std::string> showGrants(const std::string& userName) const {
        auto it = users_.find(userName);
        if (it == users_.end())
            throw std::runtime_error("User '" + userName + "' nicht gefunden");
        std::vector<std::string> result;
        for (const auto& [tbl, privSet] : it->second.grants) {
            if (privSet.empty()) continue;  // skip tables with no active grants
            std::string line = tbl + ": ";
            bool first = true;
            for (const auto& p : privSet) {
                if (!first) line += ", ";
                line += p;
                first = false;
            }
            result.push_back(line);
        }
        return result;
    }

    // Check if current user has privilege on table. Throws if denied.
    void checkPrivilege(const std::string& operation, const std::string& tableName) const {
        if (currentUser_ == "root") return; // superuser
        // Skip check for CTE temp tables
        if (tempTableNames_.count(tableName)) return;
        // Skip check for views
        if (views_.count(tableName)) return;
        // Only check for regular tables
        auto it = users_.find(currentUser_);
        if (it == users_.end())
            throw std::runtime_error("Kein aktiver Benutzer");
        const auto& grants = it->second.grants;
        auto tblIt = grants.find(tableName);
        if (tblIt != grants.end()) {
            const auto& privSet = tblIt->second;
            if (privSet.count("ALL") || privSet.count(operation)) return;
        }
        throw std::runtime_error("Keine " + operation + "-Berechtigung fuer " + currentUser_);
    }

    void saveUsers(const std::string& path = "database.users") const {
        std::ofstream f(path);
        for (const auto& [n, u] : users_) {
            if (n == "root") continue; // don't persist root
            f << u.name << "\t" << u.passwordHash << "\n";
            for (const auto& [tbl, privSet] : u.grants) {
                f << "  " << tbl << ":";
                bool first = true;
                for (const auto& p : privSet) {
                    if (!first) f << ",";
                    f << p;
                    first = false;
                }
                f << "\n";
            }
            f << "---\n";
        }
    }

    void loadUsers(const std::string& path = "database.users") {
        // Ensure root always exists
        if (users_.find("root") == users_.end()) {
            UserDef root;
            root.name = "root";
            root.passwordHash = 0;
            users_["root"] = root;
        }
        std::ifstream f(path);
        if (!f) return;
        std::string line;
        UserDef current;
        bool inUser = false;
        while (std::getline(f, line)) {
            if (line == "---") {
                if (inUser) users_[current.name] = current;
                current = UserDef{};
                inUser = false;
            } else if (line.size() >= 2 && line[0] == ' ' && line[1] == ' ') {
                // grant line: "  tableName:PRIV1,PRIV2"
                std::string rest = line.substr(2);
                size_t colon = rest.find(':');
                if (colon != std::string::npos) {
                    std::string tbl = rest.substr(0, colon);
                    std::string privStr = rest.substr(colon + 1);
                    std::string p;
                    for (char c : privStr) {
                        if (c == ',') { if (!p.empty()) current.grants[tbl].insert(p); p = ""; }
                        else p += c;
                    }
                    if (!p.empty()) current.grants[tbl].insert(p);
                }
            } else {
                // user line: "name\thash"
                size_t tab = line.find('\t');
                if (tab != std::string::npos) {
                    current.name = line.substr(0, tab);
                    try { current.passwordHash = std::stoull(line.substr(tab + 1)); } catch (...) {}
                    inUser = true;
                }
            }
        }
        if (inUser) users_[current.name] = current;
    }

    // ── Phase 75: Row-Level Security public methods ───────────
    void enableRls(const std::string& table) { rlsEnabled_.insert(resolveTableName(table)); }
    void disableRls(const std::string& table) { rlsEnabled_.erase(resolveTableName(table)); }
    bool isRlsEnabled(const std::string& table) const { return rlsEnabled_.count(table) > 0; }

    void createRlsPolicy(const RlsPolicy& pin) {
        RlsPolicy p = pin;
        p.table = resolveTableName(p.table);
        rlsPolicies_[p.table].push_back(p);
    }
    void dropRlsPolicy(const std::string& policyName, const std::string& table) {
        auto key = resolveTableName(table);
        auto it = rlsPolicies_.find(key);
        if (it == rlsPolicies_.end()) return;
        auto& v = it->second;
        v.erase(std::remove_if(v.begin(), v.end(),
            [&](const RlsPolicy& p){ return p.name == policyName; }), v.end());
    }
    void showPolicies(const std::string& table) const {
        auto key = resolveTableName(table);
        auto it = rlsPolicies_.find(key);
        std::cout << "Policies on " << table << ":\n";
        if (it == rlsPolicies_.end() || it->second.empty()) {
            std::cout << "  (none)\n";
            return;
        }
        for (const auto& p : it->second)
            std::cout << "  " << p.name << " | " << p.command
                      << " | TO " << p.role
                      << " | USING (" << p.usingExpr << ")\n";
    }
    void saveRls(const std::string& path) const {
        std::ofstream f(path);
        for (const auto& tbl : rlsEnabled_)
            f << "ENABLED " << tbl << "\n";
        for (const auto& [tbl, policies] : rlsPolicies_)
            for (const auto& p : policies)
                f << "POLICY " << p.name << "|" << p.table << "|"
                  << p.command << "|" << p.role << "|" << p.usingExpr << "\n";
    }
    void loadRls(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) return;
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("ENABLED ", 0) == 0)
                rlsEnabled_.insert(line.substr(8));
            else if (line.rfind("POLICY ", 0) == 0) {
                auto parts = splitPipe_(line.substr(7));
                if (parts.size() >= 5) {
                    RlsPolicy p;
                    p.name = parts[0]; p.table = parts[1];
                    p.command = parts[2]; p.role = parts[3];
                    p.usingExpr = parts[4];
                    rlsPolicies_[p.table].push_back(p);
                }
            }
        }
    }

    // Returns SQL with ? replaced by args in order
    std::string bindPrepared(const std::string& name,
                              const std::vector<std::string>& args) const {
        auto it = preparedStmts_.find(name);
        if (it == preparedStmts_.end())
            throw std::runtime_error("Prepared statement '" + name + "' nicht gefunden");
        std::string sql = it->second.sql;
        std::string result;
        size_t argIdx = 0;
        for (size_t i = 0; i < sql.size(); ++i) {
            if (sql[i] == '?') {
                if (argIdx < args.size())
                    result += args[argIdx++];
                else
                    result += "NULL";
            } else {
                result += sql[i];
            }
        }
        return result;
    }

    // ── Phase 51: Schema Management ──────────────────────────────

    void createSchema(const std::string& name) { schemas_.insert(name); }

    void loadSchema(const std::string& name) { schemas_.insert(name); }

    void dropSchema(const std::string& name) {
        if (name == "public")
            throw std::runtime_error("Schema 'public' kann nicht geloescht werden");
        std::vector<std::string> toRemove;
        for (const auto& [tname, _] : tables_)
            if (tname.size() > name.size() + 1 &&
                tname.substr(0, name.size()) == name && tname[name.size()] == '.')
                toRemove.push_back(tname);
        for (const auto& t : toRemove) tables_.erase(t);
        schemas_.erase(name);
        if (currentSchema_ == name) currentSchema_ = "public";
    }

    std::vector<std::string> showSchemas() const {
        return {schemas_.begin(), schemas_.end()};
    }

    void useSchema(const std::string& name) {
        schemas_.insert(name);
        currentSchema_ = name;
    }

    std::string getCurrentSchema() const { return currentSchema_; }

    std::vector<std::string> showTablesInSchema(const std::string& schema) const {
        std::vector<std::string> result;
        std::string prefix = schema + ".";
        for (const auto& [tname, _] : tables_)
            if (tname.size() > prefix.size() &&
                tname.substr(0, prefix.size()) == prefix)
                result.push_back(tname.substr(prefix.size()));
        std::sort(result.begin(), result.end());
        return result;
    }

    // Resolves a table name: if it already contains '.', return as-is.
    // Backward compat: if bare name exists in tables_, return it as-is.
    // Otherwise prepend currentSchema_ + "."
    std::string resolveTableName(const std::string& name) const {
        if (name.find('.') != std::string::npos) return name;
        if (tables_.count(name)) return name;
        return currentSchema_ + "." + name;
    }

    // Phase 92: Public mutable table access for COPY bulk import (bypasses constraints)
    Table& getMutableTable(const std::string& name) {
        auto key = resolveTableName(name);
        auto it = tables_.find(key);
        if (it == tables_.end())
            throw std::runtime_error("Tabelle '" + key + "' nicht gefunden.");
        return it->second;
    }

private:
    std::set<std::string>               schemas_ = {"public"};  // Phase 51: schema registry
    std::string                         currentSchema_ = "public"; // Phase 51: active schema
    std::map<std::string, FullTextIndex> fulltextIndices_; // Phase 49: index name → fulltext index
    std::map<std::string, TriggerDef>   triggers_;   // trigger name → def
    std::map<std::string, ProcedureDef> procedures_; // procedure name → def
    std::map<std::string, PreparedStmt> preparedStmts_; // Phase 45: prepared stmts

    // ── Phase 43: Trigger Execution Engine ───────────────────────

    // Helper: to-uppercase
    static std::string trgUpper(std::string s) {
        for (char& c : s)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }

    // Helper: trim whitespace
    static std::string trgTrim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    // Helper: find column index in a columns vector (case-sensitive)
    static int trgFindCol(const std::vector<Column>& cols, const std::string& name) {
        for (size_t i = 0; i < cols.size(); ++i)
            if (cols[i].name == name) return static_cast<int>(i);
        return -1;
    }

    // Compare two values with an operator (for trigger IF conditions)
    static bool trgCompare(const std::string& a, const std::string& op, const std::string& b) {
        // Try numeric
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
        // String fallback
        if (op == "=")  return a == b;
        if (op == "!=") return a != b;
        if (op == "<")  return a <  b;
        if (op == ">")  return a >  b;
        if (op == "<=") return a <= b;
        if (op == ">=") return a >= b;
        return false;
    }

    // Substitute OLD.col and NEW.col references with actual values.
    // Also strips surrounding single quotes from literal values in the substitution.
    static std::string trgSubstituteRefs(
            const std::string& text,
            const std::vector<std::string>& newRow,
            const std::vector<std::string>& oldRow,
            const std::vector<Column>& cols)
    {
        std::string result = text;
        // Replace OLD.colname and NEW.colname
        // We do a simple scan for "OLD." and "NEW." prefixes
        // Process character by character
        std::string out;
        size_t i = 0;
        while (i < result.size()) {
            // Check for NEW. or OLD.
            bool matchedNew = false, matchedOld = false;
            if (i + 4 <= result.size()) {
                std::string pfx4 = trgUpper(result.substr(i, 4));
                if (pfx4 == "NEW.") matchedNew = true;
                if (pfx4 == "OLD.") matchedOld = true;
            }
            if (matchedNew || matchedOld) {
                // Read column name after prefix
                size_t j = i + 4;
                while (j < result.size() &&
                       (std::isalnum(static_cast<unsigned char>(result[j])) || result[j] == '_'))
                    ++j;
                std::string colName = result.substr(i + 4, j - (i + 4));
                int ci = trgFindCol(cols, colName);
                if (ci >= 0) {
                    const std::vector<std::string>& row = matchedNew ? newRow : oldRow;
                    if (static_cast<size_t>(ci) < row.size())
                        out += row[static_cast<size_t>(ci)];
                    else
                        out += "NULL";
                } else {
                    // Unknown column — keep original
                    out += result.substr(i, j - i);
                }
                i = j;
            } else {
                out += result[i++];
            }
        }
        return out;
    }

    // Parse and execute a SIGNAL statement, extracting the message.
    // Returns the message (text between single quotes, or bare word after SIGNAL).
    static std::string trgParseSignal(const std::string& stmt) {
        // Find 'message' or just the rest after SIGNAL
        std::string upper = trgUpper(stmt);
        size_t sigPos = upper.find("SIGNAL");
        if (sigPos == std::string::npos) return stmt;
        std::string rest = trgTrim(stmt.substr(sigPos + 6));
        if (!rest.empty() && rest.front() == '\'') {
            // Strip surrounding single quotes
            if (rest.size() >= 2 && rest.back() == '\'')
                return rest.substr(1, rest.size() - 2);
            return rest.substr(1);
        }
        return rest;
    }

    // Execute trigger body statements.
    // Returns false (aborted) if a SIGNAL is encountered.
    // signalMsg is set to the SIGNAL message on abort.
    // noRecurse: flag to prevent recursive trigger firing during INSERT inside trigger
    bool executeTriggerBody(
            const std::string& body,
            std::vector<std::string>& newRow,
            const std::vector<std::string>& oldRow,
            const std::vector<Column>& cols,
            std::string& signalMsg,
            bool noRecurse = false)
    {
        // Split body by semicolons
        std::vector<std::string> stmts;
        {
            std::string cur;
            bool inQ = false;
            for (char c : body) {
                if (c == '\'' && !inQ) { inQ = true; cur += c; continue; }
                if (c == '\'' && inQ)  { inQ = false; cur += c; continue; }
                if (inQ)               { cur += c; continue; }
                if (c == ';')          { stmts.push_back(trgTrim(cur)); cur.clear(); }
                else                   { cur += c; }
            }
            if (!trgTrim(cur).empty()) stmts.push_back(trgTrim(cur));
        }

        size_t i = 0;
        while (i < stmts.size()) {
            std::string stmt = trgTrim(stmts[i]);
            if (stmt.empty()) { ++i; continue; }

            std::string upper = trgUpper(stmt);

            // Skip END IF / END
            if (upper == "END IF" || upper == "END") { ++i; continue; }

            if (upper.size() >= 2 && upper.substr(0, 2) == "IF") {
                // Parse: IF NEW/OLD.col op val THEN ... [END IF]
                // Find THEN
                size_t thenPos = upper.find("THEN");
                if (thenPos == std::string::npos) { ++i; continue; }

                std::string condPart = trgTrim(stmt.substr(2, thenPos - 2));
                std::string thenPart = trgTrim(stmt.substr(thenPos + 4));

                // Evaluate condition: expect  NEW.col op val  or  OLD.col op val
                bool condResult = false;
                {
                    // Tokenize condPart
                    // Split on whitespace
                    std::vector<std::string> ctoks;
                    {
                        std::istringstream iss(condPart);
                        std::string t;
                        while (iss >> t) ctoks.push_back(t);
                    }
                    if (ctoks.size() >= 3) {
                        std::string lhs  = ctoks[0];   // NEW.col or OLD.col or literal
                        std::string op   = ctoks[1];
                        std::string rhs  = ctoks[2];
                        // Strip quotes from rhs if quoted
                        if (rhs.size() >= 2 && rhs.front() == '\'' && rhs.back() == '\'')
                            rhs = rhs.substr(1, rhs.size() - 2);

                        // Resolve lhs
                        std::string lhsVal = lhs;
                        std::string lhsUp  = trgUpper(lhs);
                        if (lhsUp.size() > 4 && lhsUp.substr(0, 4) == "NEW.") {
                            std::string colName = lhs.substr(4);
                            int ci = trgFindCol(cols, colName);
                            if (ci >= 0 && static_cast<size_t>(ci) < newRow.size())
                                lhsVal = newRow[static_cast<size_t>(ci)];
                        } else if (lhsUp.size() > 4 && lhsUp.substr(0, 4) == "OLD.") {
                            std::string colName = lhs.substr(4);
                            int ci = trgFindCol(cols, colName);
                            if (ci >= 0 && static_cast<size_t>(ci) < oldRow.size())
                                lhsVal = oldRow[static_cast<size_t>(ci)];
                        }

                        // Resolve rhs (may also be NEW.col/OLD.col)
                        std::string rhsVal = rhs;
                        std::string rhsUp  = trgUpper(rhs);
                        if (rhsUp.size() > 4 && rhsUp.substr(0, 4) == "NEW.") {
                            std::string colName = rhs.substr(4);
                            int ci = trgFindCol(cols, colName);
                            if (ci >= 0 && static_cast<size_t>(ci) < newRow.size())
                                rhsVal = newRow[static_cast<size_t>(ci)];
                        } else if (rhsUp.size() > 4 && rhsUp.substr(0, 4) == "OLD.") {
                            std::string colName = rhs.substr(4);
                            int ci = trgFindCol(cols, colName);
                            if (ci >= 0 && static_cast<size_t>(ci) < oldRow.size())
                                rhsVal = oldRow[static_cast<size_t>(ci)];
                        }

                        condResult = trgCompare(lhsVal, op, rhsVal);
                    }
                }

                if (condResult) {
                    // Execute the then-body inline if it's non-empty
                    if (!thenPart.empty()) {
                        // thenPart is the remaining text after THEN (before END IF)
                        // It may be a single statement or multiple
                        std::string innerBody = thenPart;
                        // We need to re-add a semicolon if it's not there
                        // and then call recursively
                        if (!innerBody.empty() && innerBody.back() != ';')
                            innerBody += ";";
                        if (!executeTriggerBody(innerBody, newRow, oldRow, cols, signalMsg, noRecurse))
                            return false;
                    }
                }

                ++i; continue;
            }

            if (upper.size() >= 6 && upper.substr(0, 6) == "SIGNAL") {
                signalMsg = trgParseSignal(stmt);
                return false;
            }

            if (upper.size() >= 6 && upper.substr(0, 6) == "INSERT") {
                // Substitute OLD.col and NEW.col references
                std::string resolved = trgSubstituteRefs(stmt, newRow, oldRow, cols);
                // Execute as INSERT (use the engine's internal insert without trigger firing)
                // Parse: INSERT INTO tblname VALUES (v1, v2, ...)
                if (!noRecurse) {
                    try {
                        executeTriggerInsert(resolved);
                    } catch (const std::exception& ex) {
                        // INSERT inside trigger failed — ignore silently or rethrow?
                        // For now rethrow
                        throw;
                    }
                }
                ++i; continue;
            }

            if (upper.size() >= 8 && upper.substr(0, 8) == "SET NEW.") {
                // Parse: SET NEW.col = val
                std::string rest = trgTrim(stmt.substr(8));  // after "SET NEW."
                // find '='
                size_t eqPos = rest.find('=');
                if (eqPos != std::string::npos) {
                    std::string colName = trgTrim(rest.substr(0, eqPos));
                    std::string val     = trgTrim(rest.substr(eqPos + 1));
                    // Strip single quotes from val
                    if (val.size() >= 2 && val.front() == '\'' && val.back() == '\'')
                        val = val.substr(1, val.size() - 2);
                    int ci = trgFindCol(cols, colName);
                    if (ci >= 0 && static_cast<size_t>(ci) < newRow.size())
                        newRow[static_cast<size_t>(ci)] = val;
                }
                ++i; continue;
            }

            ++i;
        }
        return true;
    }

    // Execute an INSERT statement inside a trigger body (no recursive trigger firing)
    void executeTriggerInsert(const std::string& sql) {
        // Simple parse: INSERT INTO tblname VALUES (v1, v2, ...)
        // Tokenize
        std::vector<std::string> tokens;
        {
            std::string cur;
            bool inQ = false;
            for (char c : sql) {
                if (c == '\'' && !inQ) { inQ = true; cur += c; continue; }
                if (c == '\'' && inQ)  { inQ = false; cur += c; continue; }
                if (inQ)               { cur += c; continue; }
                if (c == ' ' || c == '\t') {
                    if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
                } else if (c == '(' || c == ')' || c == ',') {
                    if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
                    tokens.push_back(std::string(1, c));
                } else {
                    cur += c;
                }
            }
            if (!cur.empty()) tokens.push_back(cur);
        }

        // Find INSERT INTO tblname VALUES (...)
        // tokens[0]=INSERT, [1]=INTO, [2]=tblname, [3]=VALUES, [4]=(, ...values..., )
        if (tokens.size() < 5) return;
        if (trgUpper(tokens[0]) != "INSERT") return;
        if (trgUpper(tokens[1]) != "INTO") return;
        std::string tblName = tokens[2];
        // Skip VALUES keyword
        size_t i = 3;
        if (i < tokens.size() && trgUpper(tokens[i]) == "VALUES") ++i;
        // Skip opening (
        if (i < tokens.size() && tokens[i] == "(") ++i;

        std::vector<std::string> vals;
        while (i < tokens.size() && tokens[i] != ")") {
            if (tokens[i] != ",") {
                std::string v = tokens[i];
                // Strip single quotes
                if (v.size() >= 2 && v.front() == '\'' && v.back() == '\'')
                    v = v.substr(1, v.size() - 2);
                vals.push_back(v);
            }
            ++i;
        }

        // Insert directly into table (bypass trigger firing)
        auto resolvedTbl = resolveTableName(tblName);
        if (!tables_.count(resolvedTbl)) return;
        Table& tbl = tables_.at(resolvedTbl);
        applyDefaults(tbl, vals);
        applyAutoInc(tbl, vals);
        tbl.insert(Row(vals));
    }

    // Fire all triggers of the given timing/event for a table.
    // Returns true if operation should proceed, false if SIGNAL aborted.
    // Only fires ROW-level triggers (granularity == "ROW" or empty).
    bool fireAllTriggers(
            const std::string& timing,
            const std::string& event,
            const std::string& tableName,
            std::vector<std::string>& newRow,
            const std::vector<std::string>& oldRow,
            std::string& signalMsg)
    {
        auto resolvedTbl = resolveTableName(tableName);
        if (!tables_.count(resolvedTbl)) return true;
        const std::vector<Column>& cols = tables_.at(resolvedTbl).columns();

        for (auto& [trgName, trg] : triggers_) {
            if (trgUpper(trg.timing)   != trgUpper(timing))   continue;
            if (trgUpper(trg.event)    != trgUpper(event))    continue;
            if (trg.tableName != tableName)                    continue;
            // Phase 93: skip STATEMENT-level triggers here
            if (trgUpper(trg.granularity) == "STATEMENT")     continue;

            if (!executeTriggerBody(trg.body, newRow, oldRow, cols, signalMsg, false))
                return false;
        }
        return true;
    }

    // ── Tabellen-Zugriff ──────────────────────────────────────

    // Phase 63: build a virtual INFORMATION_SCHEMA table on demand
    Table buildInfoSchemaTable(const std::string& rawName) const {
        // Extract the sub-table name (part after the dot)
        std::string low = rawName;
        for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string sub;
        auto dot = low.find('.');
        sub = (dot != std::string::npos) ? low.substr(dot + 1) : "tables";

        // Helper: split "schema.name" into (schema, bareName)
        auto splitName = [](const std::string& fullName)
            -> std::pair<std::string, std::string>
        {
            auto p = fullName.find('.');
            if (p != std::string::npos)
                return {fullName.substr(0, p), fullName.substr(p + 1)};
            return {"public", fullName};
        };

        if (sub == "tables") {
            Table t(rawName, {
                Column("TABLE_SCHEMA", "TEXT"),
                Column("TABLE_NAME",   "TEXT"),
                Column("TABLE_TYPE",   "TEXT"),
                Column("TABLE_ROWS",   "INT")
            });
            for (const auto& [tname, tbl] : tables_) {
                if (tempTableNames_.count(tname)) continue;
                auto [schema, bare] = splitName(tname);
                t.insert(Row({schema, bare, "BASE TABLE",
                              std::to_string(tbl.rowCount())}));
            }
            for (const auto& [vname, vsql] : views_) {
                (void)vsql;
                auto [schema, bare] = splitName(vname);
                t.insert(Row({schema, bare, "VIEW", "0"}));
            }
            return t;
        }

        if (sub == "columns") {
            Table t(rawName, {
                Column("TABLE_SCHEMA",     "TEXT"),
                Column("TABLE_NAME",       "TEXT"),
                Column("COLUMN_NAME",      "TEXT"),
                Column("ORDINAL_POSITION", "INT"),
                Column("DATA_TYPE",        "TEXT"),
                Column("IS_NULLABLE",      "TEXT"),
                Column("COLUMN_KEY",       "TEXT"),
                Column("EXTRA",            "TEXT")
            });
            for (const auto& [tname, tbl] : tables_) {
                if (tempTableNames_.count(tname)) continue;
                auto [schema, bare] = splitName(tname);
                const auto& cols = tbl.columns();
                for (size_t i = 0; i < cols.size(); ++i) {
                    const Column& c = cols[i];
                    std::string colKey;
                    if (c.isPrimaryKey)  colKey = "PRI";
                    else if (c.isUnique) colKey = "UNI";
                    std::string extra = c.autoIncrement ? "auto_increment" : "";
                    std::string isNull = (c.notNull || c.isPrimaryKey) ? "NO" : "YES";
                    t.insert(Row({schema, bare, c.name,
                                  std::to_string(i + 1), c.type,
                                  isNull, colKey, extra}));
                }
            }
            return t;
        }

        if (sub == "indexes" || sub == "statistics") {
            Table t(rawName, {
                Column("TABLE_SCHEMA", "TEXT"),
                Column("TABLE_NAME",   "TEXT"),
                Column("INDEX_NAME",   "TEXT"),
                Column("COLUMN_NAME",  "TEXT"),
                Column("NON_UNIQUE",   "INT"),
                Column("INDEX_TYPE",   "TEXT")
            });
            for (const auto& [tname, tbl] : tables_) {
                if (tempTableNames_.count(tname)) continue;
                auto [schema, bare] = splitName(tname);
                for (const auto& idx : tbl.getIndexes()) {
                    t.insert(Row({schema, bare, idx.indexName,
                                  idx.colName, "1", "BTREE"}));
                }
            }
            return t;
        }

        if (sub == "views") {
            Table t(rawName, {
                Column("TABLE_SCHEMA",    "TEXT"),
                Column("VIEW_NAME",       "TEXT"),
                Column("VIEW_DEFINITION", "TEXT")
            });
            for (const auto& [vname, vsql] : views_) {
                auto [schema, bare] = splitName(vname);
                t.insert(Row({schema, bare, vsql}));
            }
            return t;
        }

        if (sub == "triggers") {
            Table t(rawName, {
                Column("TRIGGER_SCHEMA",     "TEXT"),
                Column("TRIGGER_NAME",       "TEXT"),
                Column("EVENT_MANIPULATION", "TEXT"),
                Column("EVENT_OBJECT_TABLE", "TEXT"),
                Column("ACTION_TIMING",      "TEXT"),
                Column("ACTION_STATEMENT",   "TEXT")
            });
            for (const auto& [trgName, trg] : triggers_) {
                (void)trgName;
                auto [schema, bare] = splitName(trg.tableName);
                (void)bare;
                t.insert(Row({schema, trg.name, trg.event,
                              trg.tableName, trg.timing, trg.body}));
            }
            return t;
        }

        if (sub == "routines") {
            Table t(rawName, {
                Column("ROUTINE_SCHEMA",     "TEXT"),
                Column("ROUTINE_NAME",       "TEXT"),
                Column("ROUTINE_TYPE",       "TEXT"),
                Column("ROUTINE_DEFINITION", "TEXT")
            });
            for (const auto& [pname, proc] : procedures_) {
                (void)pname;
                t.insert(Row({"public", proc.name, "PROCEDURE", proc.body}));
            }
            return t;
        }

        if (sub == "schemata") {
            Table t(rawName, {
                Column("SCHEMA_NAME",               "TEXT"),
                Column("DEFAULT_CHARACTER_SET_NAME", "TEXT")
            });
            for (const auto& s : schemas_) {
                t.insert(Row({s, "utf8mb4"}));
            }
            return t;
        }

        if (sub == "partitions") {
            Table t(rawName, {
                Column("TABLE_SCHEMA",         "TEXT"),
                Column("TABLE_NAME",           "TEXT"),
                Column("PARTITION_NAME",       "TEXT"),
                Column("PARTITION_METHOD",     "TEXT"),
                Column("PARTITION_EXPRESSION", "TEXT"),
                Column("TABLE_ROWS",           "INT")
            });
            for (const auto& [tname, tbl] : tables_) {
                if (tempTableNames_.count(tname)) continue;
                const PartitionInfo& pi = tbl.getPartitionInfo();
                if (!pi.hasPartitions()) continue;
                auto [schema, bare] = splitName(tname);
                std::string method;
                if      (pi.type == PartitionType::RANGE) method = "RANGE";
                else if (pi.type == PartitionType::LIST)  method = "LIST";
                else if (pi.type == PartitionType::HASH)  method = "HASH";
                auto stats = tbl.getPartitionStats();
                for (const auto& [pname, cnt] : stats) {
                    std::string expr;
                    if (pi.type == PartitionType::RANGE) {
                        for (const auto& r : pi.ranges)
                            if (r.name == pname) { expr = "< " + r.limitStr; break; }
                    } else if (pi.type == PartitionType::LIST) {
                        for (const auto& l : pi.lists) {
                            if (l.name == pname) {
                                for (size_t i = 0; i < l.values.size(); ++i) {
                                    if (i) expr += ",";
                                    expr += l.values[i];
                                }
                                break;
                            }
                        }
                    } else {
                        expr = "HASH(" + pi.column + ")";
                    }
                    t.insert(Row({schema, bare, pname, method, expr,
                                  std::to_string(cnt)}));
                }
            }
            return t;
        }

        if (sub == "events") {
            Table t(rawName, {
                Column("EVENT_SCHEMA",     "TEXT"),
                Column("EVENT_NAME",       "TEXT"),
                Column("EVENT_DEFINITION", "TEXT"),
                Column("INTERVAL_VALUE",   "TEXT"),
                Column("STATUS",           "TEXT")
            });
            if (g_eventScheduler) {
                for (const auto& ev : g_eventScheduler->getEvents()) {
                    std::string interval;
                    if (ev.recurring) {
                        interval = "EVERY " + std::to_string(ev.intervalSecs) + "s";
                        if (ev.hasAt) interval += " AT " + ev.atTime;
                    } else {
                        interval = "AT " + ev.atTime;
                    }
                    std::string status = ev.enabled ? "ENABLED" : "DISABLED";
                    t.insert(Row({"public", ev.name, ev.sql, interval, status}));
                }
            }
            return t;
        }

        if (sub == "user_privileges") {
            Table t(rawName, {
                Column("GRANTEE",        "TEXT"),
                Column("TABLE_NAME",     "TEXT"),
                Column("PRIVILEGE_TYPE", "TEXT")
            });
            for (const auto& [uname, user] : users_) {
                for (const auto& [tname, privs] : user.grants) {
                    for (const auto& priv : privs) {
                        t.insert(Row({uname, tname, priv}));
                    }
                }
            }
            return t;
        }

        throw std::runtime_error(
            "Unknown INFORMATION_SCHEMA table: '" + sub + "'");
    }

    Table& getTable(const std::string& n) {
        auto it = tables_.find(n);
        if (it == tables_.end())
            throw std::runtime_error("Tabelle '" + n + "' nicht gefunden.");
        return it->second;
    }
    const Table& getTable(const std::string& n) const {
        if (isInfoSchemaName(n)) {
            infoSchemaCache_[n] = buildInfoSchemaTable(n);
            return infoSchemaCache_[n];
        }
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
    // Phase 31: Spaltenindex nach Name (auch qualifiziert), gibt -1 zurück
    static int findColIdx(const Table& t, const std::string& name) {
        const auto& cols = t.columns();
        for (size_t i = 0; i < cols.size(); ++i)
            if (cols[i].name == name) return static_cast<int>(i);
        // Suffix-Match: "t.col" passt auf "col"
        auto dot = name.find('.');
        std::string raw = dot != std::string::npos ? name.substr(dot + 1) : name;
        for (size_t i = 0; i < cols.size(); ++i) {
            auto d2 = cols[i].name.find('.');
            std::string suf = d2 != std::string::npos
                              ? cols[i].name.substr(d2 + 1) : cols[i].name;
            if (suf == raw) return static_cast<int>(i);
        }
        return -1;
    }

    // Phase 31: CASE WHEN-Ausdruck für eine Zeile auswerten
    static std::string evalCase(const SelectItem& item,
                                const Table& tbl, const Row& row) {
        for (const auto& wh : item.caseWhen) {
            int ci = findColIdx(tbl, wh.col);
            if (ci < 0) continue;
            const std::string& rv =
                static_cast<size_t>(ci) < row.values.size()
                ? row.values[static_cast<size_t>(ci)] : "";
            // Numerischer Vergleich, fallback auf String
            bool match = false;
            try {
                double da = std::stod(rv), db = std::stod(wh.val);
                if      (wh.op == "=")  match = (da == db);
                else if (wh.op == "!=") match = (da != db);
                else if (wh.op == "<")  match = (da <  db);
                else if (wh.op == ">")  match = (da >  db);
                else if (wh.op == "<=") match = (da <= db);
                else if (wh.op == ">=") match = (da >= db);
            } catch (...) {
                if      (wh.op == "=")  match = (rv == wh.val);
                else if (wh.op == "!=") match = (rv != wh.val);
                else if (wh.op == "<")  match = (rv <  wh.val);
                else if (wh.op == ">")  match = (rv >  wh.val);
                else if (wh.op == "<=") match = (rv <= wh.val);
                else if (wh.op == ">=") match = (rv >= wh.val);
            }
            if (match) return wh.result;
        }
        return item.caseElse;
    }

    // Phase 40: toUpper helper (used by static methods in Engine)
    static std::string toUpperStatic(std::string s) {
        for (char& c : s)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }

    // Phase 90: Thread-local extension manager pointer for evaluateFunc dispatch
    // Set before query execution by non-static wrappers.
    static ExtensionManager*& tl_extMgr() {
        static thread_local ExtensionManager* ptr = nullptr;
        return ptr;
    }

    // Phase 32: String-Funktion auswerten
    // Argumente: Spaltenname → Zellwert; 'literal' oder literal → direkt
    static std::string evaluateFunc(const std::string& fn,
                                     const std::vector<std::string>& args,
                                     const Table& tbl, const Row& row) {
        // Phase 90: Check extension manager first
        {
            ExtensionManager* em = tl_extMgr();
            if (em) {
                std::string fnUp = fn;
                for (char& c : fnUp) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                auto res = em->tryEvaluate(fnUp, args);
                if (res.first) return res.second;
            }
        }

        // Argument auflösen: Spalte → Wert, 'literal' → literal, sonst direkt
        // Phase 40: wenn arg mehrere Tokens enthält (z.B. "CAST ( id AS TEXT )"),
        // via evalExprStr auswerten.
        auto resolveArg = [&](const std::string& a) -> std::string {
            if (a.size() >= 2 && a.front() == '\'' && a.back() == '\'')
                return a.substr(1, a.size() - 2);  // 'text' → text
            // Multi-token: delegate to evalExprStr (handles nested CAST, ROUND, etc.)
            {
                bool hasSpace = false;
                for (char c : a) if (c == ' ') { hasSpace = true; break; }
                if (hasSpace) return evalExprStr(a, tbl.columns(), row);
            }
            int ci = findColIdx(tbl, a);
            if (ci >= 0 && static_cast<size_t>(ci) < row.values.size())
                return row.values[static_cast<size_t>(ci)];
            return a;  // unquoted literal
        };

        if (fn == "UPPER") {
            std::string v = args.empty() ? "" : resolveArg(args[0]);
            for (char& c : v) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return v;
        }
        if (fn == "LOWER") {
            std::string v = args.empty() ? "" : resolveArg(args[0]);
            for (char& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return v;
        }
        if (fn == "LENGTH") {
            std::string v = args.empty() ? "" : resolveArg(args[0]);
            return std::to_string(v.size());
        }
        if (fn == "CONCAT") {
            std::string result;
            for (const auto& a : args) result += resolveArg(a);
            return result;
        }
        if (fn == "SUBSTR") {
            if (args.size() < 3) return args.empty() ? "" : resolveArg(args[0]);
            std::string v = resolveArg(args[0]);
            int pos = 1, len = -1;
            try { pos = std::stoi(resolveArg(args[1])); } catch (...) {}
            try { len = std::stoi(resolveArg(args[2])); } catch (...) {}
            if (pos < 1) pos = 1;
            size_t start = static_cast<size_t>(pos - 1);
            if (start >= v.size()) return "";
            return len < 0 ? v.substr(start)
                           : v.substr(start, static_cast<size_t>(len));
        }
        if (fn == "TRIM") {
            std::string v = args.empty() ? "" : resolveArg(args[0]);
            size_t a = v.find_first_not_of(" \t");
            if (a == std::string::npos) return "";
            size_t b = v.find_last_not_of(" \t");
            return v.substr(a, b - a + 1);
        }
        if (fn == "REPLACE") {
            if (args.size() < 3) return args.empty() ? "" : resolveArg(args[0]);
            std::string v   = resolveArg(args[0]);
            std::string old = resolveArg(args[1]);
            std::string neu = resolveArg(args[2]);
            if (old.empty()) return v;
            std::string res;
            size_t pos = 0;
            while (pos <= v.size()) {
                auto found = v.find(old, pos);
                if (found == std::string::npos) { res += v.substr(pos); break; }
                res += v.substr(pos, found - pos);
                res += neu;
                pos = found + old.size();
            }
            return res;
        }
        // ── Phase 33: Math-Funktionen ────────────────────────────
        if (fn == "ABS") {
            if (args.empty()) return "0";
            try {
                double v = std::stod(resolveArg(args[0]));
                return formatNum(std::abs(v));
            } catch (...) { return "NaN"; }
        }
        if (fn == "ROUND") {
            if (args.empty()) return "0";
            try {
                double v = std::stod(resolveArg(args[0]));
                if (args.size() >= 2) {
                    int dec = 0;
                    try { dec = std::stoi(resolveArg(args[1])); } catch (...) {}
                    double factor = std::pow(10.0, dec);
                    double rounded = std::round(v * factor) / factor;
                    if (dec <= 0) return formatNum(rounded);
                    // Dezimalstellen formatieren
                    std::string s = std::to_string(rounded);
                    // Auf dec Stellen nach dem Punkt kürzen
                    auto dot = s.find('.');
                    if (dot != std::string::npos && s.size() > dot + 1 + static_cast<size_t>(dec))
                        s = s.substr(0, dot + 1 + static_cast<size_t>(dec));
                    // Trailing zeros entfernen bis auf dec Stellen
                    return s;
                }
                return formatNum(std::round(v));
            } catch (...) { return "NaN"; }
        }
        if (fn == "MOD") {
            if (args.size() < 2) return "0";
            try {
                double a = std::stod(resolveArg(args[0]));
                double b = std::stod(resolveArg(args[1]));
                if (b == 0.0) return "NaN";
                return formatNum(std::fmod(a, b));
            } catch (...) { return "NaN"; }
        }
        if (fn == "POWER") {
            if (args.size() < 2) return "0";
            try {
                double base = std::stod(resolveArg(args[0]));
                double exp  = std::stod(resolveArg(args[1]));
                return formatNum(std::pow(base, exp));
            } catch (...) { return "NaN"; }
        }
        if (fn == "SQRT") {
            if (args.empty()) return "0";
            try {
                double v = std::stod(resolveArg(args[0]));
                if (v < 0.0) return "NaN";
                double r = std::sqrt(v);
                // Ganzzahl falls exakt, sonst mit Dezimalstellen
                long long iv = static_cast<long long>(r);
                if (static_cast<double>(iv) == r) return std::to_string(iv);
                std::string s = std::to_string(r);
                auto dot = s.find('.');
                if (dot != std::string::npos) {
                    size_t last = s.find_last_not_of('0');
                    if (last == dot) return s.substr(0, dot);
                    return s.substr(0, last + 1);
                }
                return s;
            } catch (...) { return "NaN"; }
        }
        if (fn == "CEIL") {
            if (args.empty()) return "0";
            try {
                double v = std::stod(resolveArg(args[0]));
                return formatNum(std::ceil(v));
            } catch (...) { return "NaN"; }
        }
        if (fn == "FLOOR") {
            if (args.empty()) return "0";
            try {
                double v = std::stod(resolveArg(args[0]));
                return formatNum(std::floor(v));
            } catch (...) { return "NaN"; }
        }
        // ── Phase 34: NULL-Behandlung ────────────────────────────
        if (fn == "IFNULL") {
            // IFNULL(val, default) — val wenn nicht NULL, sonst default
            if (args.size() < 2) return args.empty() ? "NULL" : resolveArg(args[0]);
            std::string v = resolveArg(args[0]);
            return (v == "NULL") ? resolveArg(args[1]) : v;
        }
        if (fn == "COALESCE") {
            // COALESCE(v1, v2, ...) — erstes nicht-NULL Argument
            for (const auto& a : args) {
                std::string v = resolveArg(a);
                if (v != "NULL") return v;
            }
            return "NULL";
        }
        // ── Phase 40: CAST ───────────────────────────────────────
        if (fn == "CAST") {
            if (args.size() < 2) return "";
            // args[0] = inner expression (token string), args[1] = target type
            std::string inner = evalExprStr(args[0], tbl.columns(), row);
            std::string type  = toUpperStatic(args[1]);
            if (type == "INT") {
                try { return std::to_string((long long)std::stold(inner)); }
                catch (...) { return "0"; }
            } else if (type == "REAL") {
                try {
                    double d = std::stod(inner);
                    std::ostringstream oss;
                    oss << d;
                    return oss.str();
                } catch (...) { return "0.0"; }
            } else {  // TEXT
                return inner;
            }
        }

        // ── Phase 55: DATE/TIME-Funktionen ──────────────────────
        if (fn == "NOW" || fn == "CURRENT_TIMESTAMP")
            return milansql::dateutils::currentDatetimeStr();
        if (fn == "CURDATE" || fn == "CURRENT_DATE")
            return milansql::dateutils::currentDateStr();
        if (fn == "CURTIME" || fn == "CURRENT_TIME")
            return milansql::dateutils::currentTimeStr();

        if (fn == "YEAR" || fn == "MONTH" || fn == "DAY" ||
            fn == "HOUR" || fn == "MINUTE" || fn == "SECOND") {
            if (args.empty()) return "NULL";
            return milansql::dateutils::extractPart(resolveArg(args[0]), fn);
        }

        if (fn == "DATE") {
            // DATE(datetime) → nur Datumsteil "YYYY-MM-DD"
            if (args.empty()) return "NULL";
            std::string v = milansql::dateutils::stripQuotes(resolveArg(args[0]));
            if (v.size() >= 10) return v.substr(0, 10);
            return "NULL";
        }
        if (fn == "TIME") {
            // TIME(datetime) → nur Zeitteil "HH:MM:SS"
            if (args.empty()) return "NULL";
            std::string v = milansql::dateutils::stripQuotes(resolveArg(args[0]));
            if (v.size() >= 19) return v.substr(11, 8);
            if (v.size() >= 5 && v[2] == ':') return v;
            return "NULL";
        }

        if (fn == "DATEDIFF") {
            // DATEDIFF(date1, date2) → Tage
            if (args.size() < 2) return "NULL";
            return milansql::dateutils::dateDiff(resolveArg(args[0]), resolveArg(args[1]));
        }

        if (fn == "DATE_ADD" || fn == "DATE_SUB") {
            // DATE_ADD(date, INTERVAL n UNIT)
            // Parser may produce:
            //   args[0]=date, args[1]="INTERVAL n UNIT"  (2 args)
            // or:
            //   args[0]=date, args[1]=n, args[2]=unit    (3 args)
            if (args.size() < 2) return "NULL";
            std::string dateVal = resolveArg(args[0]);
            long n = 0;
            std::string unit;
            if (args.size() >= 3) {
                // 3-arg form
                try { n = std::stol(resolveArg(args[1])); } catch (...) { return "NULL"; }
                unit = resolveArg(args[2]);
            } else {
                // 2-arg form: args[1] = "INTERVAL n UNIT" (possibly with leading INTERVAL token)
                std::string intervalStr = args[1];
                // strip leading "INTERVAL " (case-insensitive)
                if (intervalStr.size() > 9) {
                    std::string pfx = intervalStr.substr(0, 9);
                    for (char& c : pfx) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    if (pfx == "INTERVAL ") intervalStr = intervalStr.substr(9);
                }
                // trim
                while (!intervalStr.empty() && intervalStr.front() == ' ') intervalStr.erase(intervalStr.begin());
                while (!intervalStr.empty() && intervalStr.back()  == ' ') intervalStr.pop_back();
                // split on space: first = n, second = unit
                auto sp = intervalStr.find(' ');
                if (sp == std::string::npos) return "NULL";
                try { n = std::stol(intervalStr.substr(0, sp)); } catch (...) { return "NULL"; }
                unit = intervalStr.substr(sp + 1);
                while (!unit.empty() && unit.front() == ' ') unit.erase(unit.begin());
                while (!unit.empty() && unit.back()  == ' ') unit.pop_back();
            }
            if (fn == "DATE_SUB") n = -n;
            return milansql::dateutils::dateAdd(dateVal, n, unit);
        }

        if (fn == "DATE_FORMAT") {
            if (args.size() < 2) return "NULL";
            return milansql::dateutils::dateFormat(resolveArg(args[0]), resolveArg(args[1]));
        }

        // ── Phase 56: JSON-Funktionen ────────────────────────────
        if (fn == "JSON_EXTRACT") {
            if (args.size() < 2) return "NULL";
            std::string jsonStr = resolveArg(args[0]);
            // path: strip single quotes
            std::string path = resolveArg(args[1]);
            if (path.size() >= 2 && path.front() == '\'' && path.back() == '\'')
                path = path.substr(1, path.size() - 2);
            return milansql::jsonutils::extract(jsonStr, path);
        }
        if (fn == "JSON_SET") {
            if (args.size() < 3) return "NULL";
            std::string jsonStr = resolveArg(args[0]);
            std::string path    = resolveArg(args[1]);
            if (path.size() >= 2 && path.front() == '\'' && path.back() == '\'')
                path = path.substr(1, path.size() - 2);
            std::string newVal  = resolveArg(args[2]);
            return milansql::jsonutils::set(jsonStr, path, newVal);
        }
        if (fn == "JSON_KEYS") {
            if (args.empty()) return "NULL";
            return milansql::jsonutils::keys(resolveArg(args[0]));
        }
        if (fn == "JSON_LENGTH") {
            if (args.empty()) return "NULL";
            return milansql::jsonutils::length(resolveArg(args[0]));
        }
        if (fn == "JSON_CONTAINS") {
            // JSON_CONTAINS(json, val[, path])
            if (args.size() < 2) return "0";
            std::string jsonStr = resolveArg(args[0]);
            std::string val     = resolveArg(args[1]);
            std::string path    = "$";
            if (args.size() >= 3) {
                path = resolveArg(args[2]);
                if (path.size() >= 2 && path.front() == '\'' && path.back() == '\'')
                    path = path.substr(1, path.size() - 2);
            }
            return milansql::jsonutils::contains(jsonStr, val, path);
        }
        if (fn == "JSON_TYPE") {
            if (args.empty()) return "NULL";
            return milansql::jsonutils::type(resolveArg(args[0]));
        }
        if (fn == "JSON_VALID") {
            if (args.empty()) return "0";
            return milansql::jsonutils::isValid(resolveArg(args[0])) ? "1" : "0";
        }

        // ── Phase 64: REGEXP-Funktionen ──────────────────────────────
        if (fn == "REGEXP_REPLACE") {
            if (args.size() < 3) return args.empty() ? "" : resolveArg(args[0]);
            std::string str     = milansql::dateutils::stripQuotes(resolveArg(args[0]));
            std::string pattern = resolveArg(args[1]);
            std::string repl    = resolveArg(args[2]);
            try {
                return std::regex_replace(str, std::regex(pattern), repl);
            } catch (...) { return str; }
        }
        if (fn == "REGEXP_EXTRACT") {
            if (args.size() < 2) return "";
            std::string str     = milansql::dateutils::stripQuotes(resolveArg(args[0]));
            std::string pattern = resolveArg(args[1]);
            try {
                std::smatch m;
                if (std::regex_search(str, m, std::regex(pattern)))
                    return m[0].str();
            } catch (...) {}
            return "";
        }

        // ── Phase 70: Spatial Functions ─────────────────────────
        // Resolve a POINT argument: either column value or literal POINT(...)
        auto resolvePointArg = [&](const std::string& a) -> std::string {
            // First try as a space-separated token expression (POINT ( lat lng ))
            // Reconstruct if it looks like a POINT call
            std::string val = resolveArg(a);
            if (SpatialUtils::isPointLiteral(val)) return val;
            // If the arg itself contains POINT(, rebuild it
            // Check if a is a multi-token "POINT ( lat lng )" style
            if (a.find("POINT") != std::string::npos || a.find("point") != std::string::npos) {
                // rebuild: strip spaces to form POINT(lat lng)
                std::string rebuilt;
                bool inWord = false;
                for (char c : a) {
                    if (c == ' ' || c == '\t') { if (!inWord) continue; rebuilt += c; }
                    else { inWord = true; rebuilt += c; }
                }
                return rebuilt;
            }
            return val;
        };

        if (fn == "ST_DISTANCE") {
            if (args.size() < 2) return "0";
            std::string p1 = resolvePointArg(args[0]);
            std::string p2 = resolvePointArg(args[1]);
            return SpatialUtils::stDistance(p1, p2);
        }
        if (fn == "ST_X") {
            if (args.empty()) return "0";
            return SpatialUtils::stX(resolvePointArg(args[0]));
        }
        if (fn == "ST_Y") {
            if (args.empty()) return "0";
            return SpatialUtils::stY(resolvePointArg(args[0]));
        }
        if (fn == "ST_ASTEXT") {
            if (args.empty()) return "";
            return SpatialUtils::stAsText(resolvePointArg(args[0]));
        }
        if (fn == "ST_WITHIN") {
            // ST_WITHIN(point, center, radius_km)
            if (args.size() < 3) return "0";
            std::string p      = resolvePointArg(args[0]);
            std::string center = resolvePointArg(args[1]);
            double radius = 0.0;
            try { radius = std::stod(resolveArg(args[2])); } catch (...) {}
            return SpatialUtils::stWithin(p, center, radius);
        }

        // ── Phase 88: Array functions ─────────────────────────────
        if (fn == "ARRAY_LENGTH") {
            if (args.empty()) return "0";
            return ArrayUtils::arrayLength(resolveArg(args[0]));
        }
        if (fn == "ARRAY_APPEND") {
            if (args.size() < 2) return args.empty() ? "{}" : resolveArg(args[0]);
            return ArrayUtils::arrayAppend(resolveArg(args[0]), resolveArg(args[1]));
        }
        if (fn == "ARRAY_REMOVE") {
            if (args.size() < 2) return args.empty() ? "{}" : resolveArg(args[0]);
            return ArrayUtils::arrayRemove(resolveArg(args[0]), resolveArg(args[1]));
        }
        if (fn == "ARRAY_CONTAINS") {
            if (args.size() < 2) return "0";
            return ArrayUtils::arrayContains(resolveArg(args[0]), resolveArg(args[1]));
        }
        if (fn == "ARRAY_GET") {
            if (args.size() < 2) return "NULL";
            int idx = 1;
            try { idx = std::stoi(resolveArg(args[1])); } catch (...) {}
            return ArrayUtils::arrayGet(resolveArg(args[0]), idx);
        }
        if (fn == "ARRAY_TO_STRING") {
            if (args.size() < 2) return args.empty() ? "" : resolveArg(args[0]);
            return ArrayUtils::arrayToString(resolveArg(args[0]), resolveArg(args[1]));
        }
        if (fn == "STRING_TO_ARRAY") {
            if (args.size() < 2) return args.empty() ? "{}" : ArrayUtils::serialize({resolveArg(args[0])});
            return ArrayUtils::stringToArray(resolveArg(args[0]), resolveArg(args[1]));
        }

        // ── Phase 97: Time-Series functions ───────────────────────
        // TIME_BUCKET(interval, ts): truncate timestamp to bucket
        // args may come as [amount, unit, tsVal] or ["amount unit", tsVal]
        if (fn == "TIME_BUCKET") {
            std::string interval, tsVal;
            if (args.size() >= 3) {
                // ["1", "DAY", "ts_value"]
                interval = resolveArg(args[0]) + " " + resolveArg(args[1]);
                tsVal    = resolveArg(args[2]);
            } else if (args.size() == 2) {
                interval = resolveArg(args[0]);
                tsVal    = resolveArg(args[1]);
            } else {
                return "NULL";
            }
            return TimeSeriesManager::timeBucket(interval, tsVal);
        }

        // ── Phase 111: pgvector functions (inline fallback) ───────────
        // These are also handled by extension manager when CREATE EXTENSION vector
        // is used. Inline fallbacks ensure they work without the extension too.
        if (fn == "L2_DISTANCE" || fn == "COSINE_SIMILARITY" ||
            fn == "INNER_PRODUCT" || fn == "VECTOR_DIMS" ||
            fn == "VECTOR_NORM" || fn == "VECTOR_ADD" ||
            fn == "VECTOR_SUB" || fn == "VECTOR_MUL") {
            // Resolve args first
            std::vector<std::string> rargs;
            rargs.reserve(args.size());
            for (const auto& a : args) rargs.push_back(resolveArg(a));
            return vector_ext::evalVector(fn, rargs);
        }

        return "";
    }

    // ── Phase 40: evalExprStr ────────────────────────────────────
    // Evaluates a space-separated token string like "CAST ( id AS TEXT )"
    // or "ROUND ( komma , 1 )" given a row and column list.
    static std::string evalExprStr(const std::string& expr,
                                    const std::vector<Column>& cols,
                                    const Row& row) {
        if (expr.empty()) return "";

        // Tokenize on spaces
        std::vector<std::string> toks;
        {
            std::istringstream iss(expr);
            std::string t;
            while (iss >> t) toks.push_back(t);
        }
        if (toks.empty()) return "";

        // Single token: column lookup or literal
        if (toks.size() == 1) {
            const std::string& tok = toks[0];
            // Strip single quotes
            if (tok.size() >= 2 && tok.front() == '\'' && tok.back() == '\'')
                return tok.substr(1, tok.size() - 2);
            // Column lookup
            for (size_t i = 0; i < cols.size(); ++i)
                if (cols[i].name == tok && i < row.values.size())
                    return row.values[i];
            return tok;  // literal
        }

        // Multi-token: check if first token is a known function followed by (
        static const std::vector<std::string> KNOWN_FUNCS =
            {"UPPER", "LOWER", "LENGTH", "CONCAT", "SUBSTR", "TRIM", "REPLACE",
             "ABS", "ROUND", "MOD", "POWER", "SQRT", "CEIL", "FLOOR",
             "IFNULL", "COALESCE", "CAST",
             // Phase 55: DATE/TIME
             "NOW", "CURDATE", "CURTIME", "DATE", "TIME",
             "YEAR", "MONTH", "DAY", "HOUR", "MINUTE", "SECOND",
             "DATEDIFF", "DATE_ADD", "DATE_SUB", "DATE_FORMAT",
             "CURRENT_TIMESTAMP", "CURRENT_DATE", "CURRENT_TIME",
             // Phase 56: JSON
             "JSON_EXTRACT", "JSON_SET", "JSON_KEYS", "JSON_LENGTH",
             "JSON_CONTAINS", "JSON_TYPE", "JSON_VALID",
             // Phase 70: Spatial
             "ST_DISTANCE", "ST_X", "ST_Y", "ST_WITHIN", "ST_ASTEXT",
             // Phase 88: Array functions
             "ARRAY_LENGTH", "ARRAY_APPEND", "ARRAY_REMOVE", "ARRAY_CONTAINS",
             "ARRAY_GET", "ARRAY_TO_STRING", "STRING_TO_ARRAY",
             // Phase 97: Time-Series
             "TIME_BUCKET",
             // Phase 111: pgvector
             "L2_DISTANCE","COSINE_SIMILARITY","INNER_PRODUCT",
             "VECTOR_DIMS","VECTOR_NORM","VECTOR_ADD","VECTOR_SUB","VECTOR_MUL"};

        std::string fn = toUpperStatic(toks[0]);
        bool isFunc = false;
        for (const auto& f : KNOWN_FUNCS) if (fn == f) { isFunc = true; break; }
        // Phase 90: also recognize functions registered by loaded extensions
        if (!isFunc) {
            ExtensionManager* em = tl_extMgr();
            if (em && em->isExtensionFunc(fn)) isFunc = true;
        }

        if (isFunc && toks.size() >= 2 && toks[1] == "(") {
            // Find matching closing paren
            int depth = 0;
            size_t closePos = toks.size();
            for (size_t k = 1; k < toks.size(); ++k) {
                if (toks[k] == "(") ++depth;
                else if (toks[k] == ")") {
                    --depth;
                    if (depth == 0) { closePos = k; break; }
                }
            }
            // Extract tokens between ( and )
            // toks[2 .. closePos-1] are the argument tokens
            std::vector<std::string> argToks(toks.begin() + 2,
                                              closePos < toks.size()
                                              ? toks.begin() + static_cast<std::ptrdiff_t>(closePos)
                                              : toks.end());

            // Split into args: for CAST split on AS at depth 0, otherwise on , at depth 0
            std::vector<std::string> funcArgs;
            bool isCast = (fn == "CAST");
            int d = 0;
            std::string current;
            for (size_t k = 0; k < argToks.size(); ++k) {
                const std::string& at = argToks[k];
                if (at == "(") { d++; current += at + " "; continue; }
                if (at == ")") { d--; current += at + " "; continue; }
                if (d == 0 && at == ",") {
                    std::string a = current;
                    while (!a.empty() && a.back() == ' ') a.pop_back();
                    if (!a.empty()) funcArgs.push_back(a);
                    current = "";
                    continue;
                }
                if (d == 0 && isCast && toUpperStatic(at) == "AS") {
                    std::string a = current;
                    while (!a.empty() && a.back() == ' ') a.pop_back();
                    if (!a.empty()) funcArgs.push_back(a);
                    current = "";
                    continue;
                }
                current += at + " ";
            }
            // push final arg
            {
                std::string a = current;
                while (!a.empty() && a.back() == ' ') a.pop_back();
                if (!a.empty()) funcArgs.push_back(a);
            }

            // Build a dummy Table for evaluateFunc (only needs columns)
            // We pass cols as a temporary Table wrapper via a trick:
            // evaluateFunc needs (fn, args, tbl, row) — build a temp table
            // Actually we can build a minimal Table with just columns
            Table tmpTbl("", cols);
            return evaluateFunc(fn, funcArgs, tmpTbl, row);
        }

        // Fallback: treat as literal (rejoin)
        std::string result;
        for (const auto& t : toks) result += t;
        return result;
    }

    // Exakter Match, dann Suffix-Match. Uses rfind for schema.table.col → col.
    // Returns true if tbl1 and tbl2 refer to the same table
    // (exact match OR one is a schema-qualified version of the other,
    //  e.g. "users" matches "public.users")
    static bool tblNamesMatch(const std::string& a, const std::string& b) {
        if (a == b) return true;
        // a is bare, b is schema-qualified: b ends with "."+a
        if (b.size() > a.size() + 1 &&
            b[b.size() - a.size() - 1] == '.' &&
            b.substr(b.size() - a.size()) == a) return true;
        // b is bare, a is schema-qualified: a ends with "."+b
        if (a.size() > b.size() + 1 &&
            a[a.size() - b.size() - 1] == '.' &&
            a.substr(a.size() - b.size()) == b) return true;
        return false;
    }

    static size_t findQualColIdx(const Table& t, const std::string& qual) {
        for (size_t i = 0; i < t.columns().size(); ++i)
            if (t.columns()[i].name == qual) return i;
        // For schema.table.col style references, use the last component as raw col name
        auto dot = qual.rfind('.');
        std::string raw = dot != std::string::npos ? qual.substr(dot + 1) : qual;
        // Also get the table prefix (everything before last dot) for matching
        std::string tblPrefix = dot != std::string::npos ? qual.substr(0, dot) : "";
        for (size_t i = 0; i < t.columns().size(); ++i) {
            const auto& cn = t.columns()[i].name;
            // cn is like "public.users.id" in qualified result table
            auto p = cn.rfind('.');
            std::string suf = p != std::string::npos ? cn.substr(p + 1) : cn;
            if (suf != raw) continue;
            // If tblPrefix specified, check that the table part matches
            // Use tblNamesMatch to handle bare vs schema-qualified names
            if (!tblPrefix.empty()) {
                std::string cnTbl = p != std::string::npos ? cn.substr(0, p) : "";
                if (!cnTbl.empty() && !tblNamesMatch(cnTbl, tblPrefix)) continue;
            }
            return i;
        }
        throw std::runtime_error(
            "JOIN ON: Spalte '" + qual + "' nicht im Ergebnis gefunden.");
    }

    // Spalte in roher Tabelle suchen (unqualifizierte Namen).
    // Entfernt Tabellen-Prefix falls vorhanden. Uses rfind for schema.table.col support.
    static size_t findRawColIdx(const Table& t, const std::string& qual) {
        auto dot = qual.rfind('.');  // use LAST dot for schema.table.col → col
        const std::string raw = dot != std::string::npos ? qual.substr(dot + 1) : qual;
        for (size_t i = 0; i < t.columns().size(); ++i)
            if (t.columns()[i].name == raw) return i;
        throw std::runtime_error(
            "JOIN ON: Spalte '" + qual + "' in rechter Tabelle nicht gefunden.");
    }

    // ── Phase 37: Skalare Subquery auswerten ─────────────────────
    std::string evalScalarSub(const ScalarSubSpec& spec,
                              const std::vector<Column>& outerCols,
                              const Row& outerRow) const {
        auto resolvedSub = resolveTableName(spec.subTable);
        if (!tables_.count(resolvedSub)) return "NULL";
        const Table& sub = tables_.at(resolvedSub);

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

        auto findOuterCI = [&](const std::string& ref) -> int {
            for (size_t i = 0; i < outerCols.size(); ++i)
                if (outerCols[i].name == ref) return (int)i;
            auto dot = ref.find('.');
            if (dot != std::string::npos) {
                std::string bare = ref.substr(dot + 1);
                for (size_t i = 0; i < outerCols.size(); ++i)
                    if (outerCols[i].name == bare) return (int)i;
            }
            return -1;
        };

        int aggCI = -1;
        if (spec.aggFunc != "COUNT" && !spec.aggFunc.empty())
            aggCI = findCI(sub, spec.aggCol);

        std::vector<std::string> matchVals;
        for (const auto& subRow : sub.rows()) {
            bool match = true;
            if (!spec.conds.empty()) {
                if (spec.whereLogic == "OR") {
                    match = false;
                    for (const auto& cond : spec.conds) {
                        int ci = findCI(sub, cond.col);
                        if (ci < 0) continue;
                        std::string val = cond.val;
                        int oci = findOuterCI(cond.val);
                        if (oci >= 0 && static_cast<size_t>(oci) < outerRow.values.size())
                            val = outerRow.values[static_cast<size_t>(oci)];
                        if (static_cast<size_t>(ci) < subRow.values.size() &&
                            compareValues(subRow.values[static_cast<size_t>(ci)], cond.op, val)) {
                            match = true; break;
                        }
                    }
                } else {  // AND
                    for (const auto& cond : spec.conds) {
                        int ci = findCI(sub, cond.col);
                        if (ci < 0) { match = false; break; }
                        std::string val = cond.val;
                        int oci = findOuterCI(cond.val);
                        if (oci >= 0 && static_cast<size_t>(oci) < outerRow.values.size())
                            val = outerRow.values[static_cast<size_t>(oci)];
                        if (!(static_cast<size_t>(ci) < subRow.values.size() &&
                              compareValues(subRow.values[static_cast<size_t>(ci)], cond.op, val))) {
                            match = false; break;
                        }
                    }
                }
            }
            if (!match) continue;

            if (spec.aggFunc == "COUNT") {
                matchVals.push_back("1");
            } else if (aggCI >= 0 && static_cast<size_t>(aggCI) < subRow.values.size()) {
                matchVals.push_back(subRow.values[static_cast<size_t>(aggCI)]);
            }
        }

        if (spec.aggFunc == "COUNT") return std::to_string(matchVals.size());
        if (matchVals.empty()) return "NULL";

        std::vector<double> nums;
        for (const auto& v : matchVals)
            try { nums.push_back(std::stod(v)); } catch (...) {}
        if (nums.empty()) return "NULL";

        if (spec.aggFunc == "SUM") {
            double s = 0; for (double v : nums) s += v; return formatNum(s);
        }
        if (spec.aggFunc == "AVG") {
            double s = 0; for (double v : nums) s += v;
            return formatNum(s / static_cast<double>(nums.size()));
        }
        if (spec.aggFunc == "MIN") return formatNum(*std::min_element(nums.begin(), nums.end()));
        if (spec.aggFunc == "MAX") return formatNum(*std::max_element(nums.begin(), nums.end()));
        return matchVals[0];
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

        // Phase 37: Multi-Bedingung path (subConds gesetzt)
        if (!spec.subConds.empty()) {
            for (const auto& subRow : sub.rows()) {
                bool rowMatch = (spec.subWhereLogic == "OR") ? false : true;
                for (const auto& sc : spec.subConds) {
                    int ci = findCI(sub, sc.col);
                    if (ci < 0) { rowMatch = (spec.subWhereLogic == "OR") ? rowMatch : false; continue; }
                    std::string val = sc.val;
                    int outerCI = findCI(outer, sc.val);
                    if (outerCI >= 0 && static_cast<size_t>(outerCI) < outerRow.values.size())
                        val = outerRow.values[static_cast<size_t>(outerCI)];
                    bool condMatch = static_cast<size_t>(ci) < subRow.values.size() &&
                                     compareValues(subRow.values[static_cast<size_t>(ci)], sc.op, val);
                    if (spec.subWhereLogic == "OR") {
                        if (condMatch) { rowMatch = true; break; }
                    } else {
                        if (!condMatch) { rowMatch = false; break; }
                    }
                }
                if (rowMatch) return c.op == "EXISTS";
            }
            return c.op == "NOT EXISTS";
        }

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
        ExtMgrGuard extGuard(const_cast<ExtensionManager*>(&extensionMgr_));
        if (conds.empty()) return true;

        auto evalOne = [&](const WhereCondition& c) -> bool {
            if (c.op == "EXISTS" || c.op == "NOT EXISTS")
                return evalExists(src, row, c);
            if (c.isScalarSub) {
                std::string subVal = evalScalarSub(c.scalarSub, src.columns(), row);
                size_t ci = colIdx(src, c.col);
                if (ci >= row.values.size()) return false;
                return compareValues(row.values[ci], c.op, subVal);
            }
            // Phase 40: LHS is a function expression (e.g. CAST(...))
            if (c.isFuncLhs) {
                std::string lhsVal = evalExprStr(c.funcLhsExpr, src.columns(), row);
                return evalCond(lhsVal, c);
            }
            // Phase 49: MATCH(...) AGAINST('query') in WHERE
            if (c.isMatchAgainst) {
                std::string text;
                for (const auto& col : c.matchCols) {
                    int ci2 = findColIdx(src, col);
                    if (ci2 >= 0 && static_cast<size_t>(ci2) < row.values.size())
                        text += " " + row.values[static_cast<size_t>(ci2)];
                }
                auto docWords = tokenizeText(text);
                std::set<std::string> docSet(docWords.begin(), docWords.end());
                auto qWords = tokenizeText(c.againstQuery);
                for (const auto& qw : qWords) {
                    if (docSet.count(qw)) return true;
                }
                return false;
            }
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

        // Phase 88: ARRAY_AGG
        if (item.aggFunc == "ARRAY_AGG") {
            size_t ci = colIdx(src, item.aggCol);
            std::vector<std::string> vals;
            vals.reserve(riList.size());
            for (size_t ri : riList) {
                const Row& row = src.rows()[ri];
                if (ci < row.values.size())
                    vals.push_back(row.values[ci]);
            }
            return ArrayUtils::serialize(vals);
        }

        // Phase 97: FIRST / LAST — return value with min/max timestamp
        // Must be checked BEFORE the generic colIdx call below (aggCol = "col1,col2")
        if (item.aggFunc == "FIRST" || item.aggFunc == "LAST") {
            // aggCol format: "valueCol,timeCol" (comma-separated)
            size_t comma = item.aggCol.find(',');
            std::string valColName = item.aggCol;
            std::string tsColName;
            if (comma != std::string::npos) {
                valColName = item.aggCol.substr(0, comma);
                tsColName  = item.aggCol.substr(comma + 1);
            }
            size_t valCI = colIdx(src, valColName);
            size_t tsCI  = tsColName.empty() ? valCI : colIdx(src, tsColName);
            std::string bestVal = "NULL";
            std::string bestTs;
            bool firstAgg = (item.aggFunc == "FIRST");
            for (size_t ri : riList) {
                const Row& row = src.rows()[ri];
                std::string tsV = (tsCI < row.values.size()) ? row.values[tsCI] : "";
                std::string v   = (valCI < row.values.size()) ? row.values[valCI] : "NULL";
                if (bestTs.empty() || (firstAgg ? tsV < bestTs : tsV > bestTs)) {
                    bestTs  = tsV;
                    bestVal = v;
                }
            }
            return bestVal;
        }

        // Phase 97: TIME_WEIGHTED_AVG — weighted average by time duration
        // Must be checked BEFORE the generic colIdx call below (aggCol = "col1,col2")
        if (item.aggFunc == "TIME_WEIGHTED_AVG") {
            size_t comma = item.aggCol.find(',');
            std::string valColName = item.aggCol;
            std::string tsColName;
            if (comma != std::string::npos) {
                valColName = item.aggCol.substr(0, comma);
                tsColName  = item.aggCol.substr(comma + 1);
            }
            size_t valCI = colIdx(src, valColName);
            size_t tsCI  = tsColName.empty() ? valCI : colIdx(src, tsColName);
            // Collect (ts, val) pairs and sort by ts
            std::vector<std::pair<std::string,double>> pts;
            for (size_t ri : riList) {
                const Row& row = src.rows()[ri];
                if (valCI < row.values.size() && tsCI < row.values.size()) {
                    double v = 0.0;
                    try { v = std::stod(row.values[valCI]); } catch (...) { continue; }
                    pts.push_back({row.values[tsCI], v});
                }
            }
            if (pts.size() < 2) {
                if (pts.size() == 1) return formatNum(pts[0].second);
                return "NULL";
            }
            std::sort(pts.begin(), pts.end(),
                      [](const std::pair<std::string,double>& a,
                         const std::pair<std::string,double>& b){ return a.first < b.first; });
            // Use index-based time (treat consecutive points as unit-duration segments)
            double wsum = 0.0;
            double totalW = static_cast<double>(pts.size() - 1);
            for (size_t k = 0; k + 1 < pts.size(); ++k)
                wsum += (pts[k].second + pts[k + 1].second) / 2.0;
            if (totalW <= 0.0) return "NULL";
            return formatNum(wsum / totalW);
        }

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
        // Phase 64: REGEXP / NOT REGEXP
        if (op == "REGEXP" || op == "NOT REGEXP") {
            try {
                const std::string pattern  = milansql::dateutils::stripQuotes(b);
                const std::string haystack = milansql::dateutils::stripQuotes(a);
                bool match = std::regex_search(haystack, std::regex(pattern));
                return (op == "REGEXP") ? match : !match;
            } catch (...) { return false; }
        }
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
        // Normalisiere Strings für Vergleich (entferne äußere Anführungszeichen)
        const std::string as = milansql::dateutils::stripQuotes(a);
        const std::string bs = milansql::dateutils::stripQuotes(b);
        if (op == "=")  return as == bs;
        if (op == "!=") return as != bs;
        if (op == "<")  return as <  bs;
        if (op == ">")  return as >  bs;
        if (op == "<=") return as <= bs;
        if (op == ">=") return as >= bs;
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
    static std::string resolveDefault(const Column& col) {
        if (!col.hasDefault) return "NULL";
        std::string defUp = col.defaultValue;
        for (char& c : defUp)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (defUp == "CURRENT_TIMESTAMP" || defUp == "NOW()")
            return milansql::dateutils::currentDatetimeStr();
        if (defUp == "CURDATE()" || defUp == "CURRENT_DATE")
            return milansql::dateutils::currentDateStr();
        if (defUp == "CURTIME()" || defUp == "CURRENT_TIME")
            return milansql::dateutils::currentTimeStr();
        return col.defaultValue;
    }

    static void applyDefaults(const Table& t, std::vector<std::string>& vals) {
        // Phase 55: in-place: "" = unspecified slot from named-column INSERT → apply default
        for (size_t i = 0; i < vals.size() && i < t.columns().size(); ++i) {
            if (vals[i].empty())
                vals[i] = resolveDefault(t.columns()[i]);
        }
        // Append missing values at the end
        while (vals.size() < t.columns().size()) {
            const Column& col = t.columns()[vals.size()];
            vals.push_back(resolveDefault(col));
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

    // ── Phase 68: Generated Columns ──────────────────────────

    // Arithmetic evaluator for generated column expressions.
    // Supports: column refs, numeric literals, +, -, *, /, parentheses.
    struct ArithEval {
        const std::vector<Column>& cols;
        const std::vector<std::string>& vals;
        const std::string& expr;
        size_t pos;

        double resolveCol(const std::string& name) const {
            for (size_t i = 0; i < cols.size() && i < vals.size(); ++i)
                if (cols[i].name == name) {
                    try { return std::stod(vals[i]); } catch (...) { return 0.0; }
                }
            return 0.0;
        }

        double parseFactor() {
            while (pos < expr.size() && expr[pos] == ' ') ++pos;
            if (pos < expr.size() && expr[pos] == '(') {
                ++pos;
                double v = parseExpr();
                while (pos < expr.size() && expr[pos] == ' ') ++pos;
                if (pos < expr.size() && expr[pos] == ')') ++pos;
                return v;
            }
            // Read token: number or identifier
            size_t start = pos;
            if (pos < expr.size() && (expr[pos] == '-' || expr[pos] == '+')) ++pos;
            while (pos < expr.size() && (std::isdigit((unsigned char)expr[pos]) || expr[pos] == '.')) ++pos;
            if (pos > start) {
                try { return std::stod(expr.substr(start, pos - start)); } catch (...) {}
            }
            pos = start;
            // identifier
            while (pos < expr.size() && (std::isalnum((unsigned char)expr[pos]) || expr[pos] == '_')) ++pos;
            if (pos > start) {
                std::string tok = expr.substr(start, pos - start);
                // check if numeric
                try { return std::stod(tok); } catch (...) {}
                return resolveCol(tok);
            }
            ++pos;
            return 0.0;
        }

        double parseTerm() {
            double lhs = parseFactor();
            while (pos < expr.size()) {
                while (pos < expr.size() && expr[pos] == ' ') ++pos;
                if (pos < expr.size() && expr[pos] == '*') { ++pos; lhs *= parseFactor(); }
                else if (pos < expr.size() && expr[pos] == '/') { ++pos; double r = parseFactor(); lhs = (r != 0.0 ? lhs / r : 0.0); }
                else break;
            }
            return lhs;
        }

        double parseExpr() {
            double lhs = parseTerm();
            while (pos < expr.size()) {
                while (pos < expr.size() && expr[pos] == ' ') ++pos;
                if (pos < expr.size() && expr[pos] == '+') { ++pos; lhs += parseTerm(); }
                else if (pos < expr.size() && expr[pos] == '-') { ++pos; lhs -= parseTerm(); }
                else break;
            }
            return lhs;
        }

        double eval() { pos = 0; return parseExpr(); }
    };

    static std::string evaluateGenExpr(const std::string& expr,
                                       const std::vector<Column>& cols,
                                       const std::vector<std::string>& vals) {
        ArithEval ev{cols, vals, expr, 0};
        double result = ev.eval();
        // Format: if integer, no decimal point; else up to 6 significant digits
        if (result == std::floor(result) && std::abs(result) < 1e15)
            return std::to_string(static_cast<long long>(result));
        std::ostringstream oss;
        oss << result;
        return oss.str();
    }

    // Compute all generated columns (STORED + VIRTUAL) for a row of vals.
    static void applyGeneratedCols(const Table& t, std::vector<std::string>& vals) {
        const auto& cols = t.columns();
        for (size_t i = 0; i < cols.size() && i < vals.size(); ++i) {
            if (!cols[i].isGenerated) continue;
            vals[i] = evaluateGenExpr(cols[i].generatedExpr, cols, vals);
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

    // ── Phase 56: JSON-Validierung ───────────────────────────────────────────
    static void checkJsonColumns(const Table& t,
                                  const std::vector<std::string>& vals) {
        for (size_t i = 0; i < t.columns().size() && i < vals.size(); ++i) {
            std::string colType = t.columns()[i].type;
            for (char& c : colType)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (colType != "JSON") continue;
            const std::string& v = vals[i];
            if (v.empty() || v == "NULL") continue;
            if (!milansql::jsonutils::isValid(v))
                throw std::runtime_error(
                    "Ungültiges JSON in Spalte '" + t.columns()[i].name + "'");
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
            auto resolvedRef = resolveTableName(fk.refTable);
            auto it = tables_.find(resolvedRef);
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
                // Use tblNamesMatch to handle bare vs schema-qualified names
                if (!tblNamesMatch(fk.refTable, parentName)) continue;
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
                Table& t = getTable(op.tableName);
                t.insert(Row(op.values));
                // Phase 71: stamp xmin on newly committed row
                if (mvccTxId_ != 0) t.stampXminLast(mvccTxId_);
                break;
            }
            case BufferedOp::Type::UPDATE_WHERE: {
                Table& t = getTable(op.tableName);
                size_t wCI = colIdx(t, op.whereCol);
                t.updateWhere(colIdx(t, op.setCol), op.setVal, wCI, op.whereVal);
                // Phase 68: recompute generated cols for affected rows
                for (auto& row : t.mutableRows())
                    if (wCI < row.values.size() && row.values[wCI] == op.whereVal)
                        applyGeneratedCols(t, row.values);
                t.rebuildIndexes();
                break;
            }
            case BufferedOp::Type::UPDATE_ALL: {
                Table& t = getTable(op.tableName);
                t.updateAll(colIdx(t, op.setCol), op.setVal);
                // Phase 68: recompute generated cols for all rows
                for (auto& row : t.mutableRows())
                    applyGeneratedCols(t, row.values);
                t.rebuildIndexes();
                break;
            }
            case BufferedOp::Type::DELETE_WHERE: {
                Table& t = getTable(op.tableName);
                if (mvccTxId_ != 0) {
                    // Phase 71: MVCC logical delete — stamp xmax instead of physical remove
                    std::size_t stamped = t.stampDeleteWhere(colIdx(t, op.whereCol), op.whereVal, mvccTxId_);
                    if (stamped) vacuumMgr_.addDeadTuples(op.tableName, stamped);  // Phase 85
                } else {
                    std::size_t del = t.deleteWhere(colIdx(t, op.whereCol), op.whereVal);
                    if (del) vacuumMgr_.addDeadTuples(op.tableName, del);  // Phase 85
                }
                break;
            }
            case BufferedOp::Type::DELETE_ALL: {
                if (mvccTxId_ != 0) {
                    // Phase 71: MVCC logical delete all
                    Table& tda = getTable(op.tableName);
                    std::size_t before = tda.rowCount();
                    tda.stampDeleteAll(mvccTxId_);
                    vacuumMgr_.addDeadTuples(op.tableName, before);  // Phase 85
                } else {
                    getTable(op.tableName).deleteAll();
                }
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

public:
    // ── Phase 98: CDC ─────────────────────────────────────────
    CdcManager& getCdcManager() { return cdcMgr_; }
    const CdcManager& getCdcManager() const { return cdcMgr_; }

    // Enable/disable CDC using the resolved table name
    void enableCdc(const std::string& tableRaw) {
        cdcMgr_.enableTable(resolveTableName(tableRaw));
    }
    void disableCdc(const std::string& tableRaw) {
        cdcMgr_.disableTable(resolveTableName(tableRaw));
    }

    bool isCdcTable(const std::string& name) const {
        return name.size() > 4 && name.substr(0, 4) == "cdc.";
    }

    // ── Phase 86: Table map accessor for statistics ───────────
    const std::map<std::string, Table>& getTables() const { return tables_; }

    // ── Phase 36: EXPLAIN ─────────────────────────────────────
    ExplainPlan buildExplain(const ExplainRequest& req) const {
        ExplainPlan plan;
        int nr = 1;
        auto addStep = [&](const std::string& op, const std::string& tbl,
                           const std::string& det, const std::string& idx = "-") {
            plan.steps.push_back({nr++, op, tbl, det, idx});
        };

        // SET_OP (UNION / INTERSECT / EXCEPT)
        if (req.isSetOp) {
            addStep("SET_OP", "-", req.setOp, "-");
            addStep("PROJECT", "-", "* (alle Spalten)", "-");
            return plan;
        }

        if (req.isJoin) {
            // Phase 113: DP Planner info for 3+ table JOINs
            if (req.joinClauses.size() >= 2) {
                // Build dpTables for planner
                std::vector<milansql::JoinTableInfo> dpTbl;
                std::string rBase = resolveTableName(req.tableName);
                size_t bRows = tables_.count(rBase) ? tables_.at(rBase).rowCount() : 0;
                dpTbl.push_back({rBase, bRows, {}});
                for (const auto& jc2 : req.joinClauses) {
                    std::string rJ = resolveTableName(jc2.table);
                    size_t jr = tables_.count(rJ) ? tables_.at(rJ).rowCount() : 0;
                    dpTbl.push_back({rJ, jr, {}});
                }
                // Conservative: each join requires previous (sequential default)
                for (size_t ji = 1; ji < dpTbl.size(); ++ji)
                    dpTbl[ji].requiredTables = {static_cast<int>(ji) - 1};

                std::string ck;
                std::vector<std::string> tn;
                for (const auto& t : dpTbl) tn.push_back(t.name);
                std::sort(tn.begin(), tn.end());
                for (const auto& n : tn) ck += n + ",";

                auto dp = milansql::g_dpPlanner().plan(dpTbl, ck);
                addStep("DP_PLAN", "-",
                    "DP Planner: " + dp.description, "-");
                // Show optimal order
                if (dp.dpUsed) {
                    std::string orderStr = dpTbl[0].name;
                    for (int idx : dp.joinOrder)
                        orderStr += " \u2192 " + dpTbl[static_cast<size_t>(idx + 1)].name;
                    addStep("DP_ORDER", "-", "Optimal order: " + orderStr, "-");
                }
            }
            // JOIN: Haupt-Tabelle scannen, dann jede JOIN-Tabelle
            addStep("SCAN", req.tableName, "FULL SCAN", "-");
            for (const auto& jc : req.joinClauses) {
                // Phase 83: determine join strategy for EXPLAIN output
                auto xColOf83 = [](const std::string& s) -> std::string {
                    auto p = s.rfind('.'); return p != std::string::npos ? s.substr(p+1) : s;
                };
                std::string rTbl83 = resolveTableName(jc.table);
                // Determine which ON side belongs to right table (mirror executeJoins logic)
                bool onRightIsRight83 = tblNamesMatch(
                    [](const std::string& s){ auto p=s.rfind('.'); return p!=std::string::npos?s.substr(0,p):std::string(); }(jc.onRight),
                    jc.table);
                std::string rightCol83 = onRightIsRight83 ? xColOf83(jc.onRight) : xColOf83(jc.onLeft);
                std::string leftCol83  = onRightIsRight83 ? xColOf83(jc.onLeft)  : xColOf83(jc.onRight);
                bool rightHasIdx83 = tables_.count(rTbl83) &&
                                     tables_.at(rTbl83).hasIndex(rightCol83);
                std::string lTbl83 = resolveTableName(req.tableName);
                bool leftHasIdx83  = tables_.count(lTbl83) &&
                                     tables_.at(lTbl83).hasIndex(leftCol83);
                size_t leftSz83  = tables_.count(lTbl83) ? tables_.at(lTbl83).rowCount() : 0;
                size_t rightSz83 = tables_.count(rTbl83) ? tables_.at(rTbl83).rowCount() : 0;
                JoinStrategy strat83 = JoinPlanner::choose(
                    leftSz83, rightSz83, leftHasIdx83, rightHasIdx83);
                std::string det = jc.joinType + " JOIN ON " + jc.onLeft + " = " + jc.onRight +
                    "  [" + JoinPlanner::description(strat83, leftSz83, rightSz83) + "]";
                addStep("JOIN", jc.table, det, "-");
            }
            // WHERE nach dem JOIN
            if (!req.whereConds.empty()) {
                std::string det;
                for (size_t i = 0; i < req.whereConds.size(); ++i) {
                    if (i > 0) det += " " + req.whereLogic + " ";
                    det += req.whereConds[i].col + " " +
                           req.whereConds[i].op  + " " + req.whereConds[i].val;
                }
                addStep("FILTER", "-", det, "-");
            }
        } else {
            // Einzelne Tabelle: Index-Check
            bool useIndex = false;
            std::string idxName = "-";
            if (!req.whereConds.empty() && req.whereConds[0].op == "=" &&
                tables_.count(resolveTableName(req.tableName))) {
                const Table& t = tables_.at(resolveTableName(req.tableName));
                if (t.hasIndex(req.whereConds[0].col)) {
                    useIndex = true;
                    // Index-Namen heraussuchen (führende Spalte)
                    for (const auto& info : t.getIndexes()) {
                        std::string leading = info.colName;
                        size_t comma = leading.find(',');
                        if (comma != std::string::npos)
                            leading = leading.substr(0, comma);
                        while (!leading.empty() && leading[0]  == ' ') leading.erase(0, 1);
                        while (!leading.empty() && leading.back() == ' ') leading.pop_back();
                        if (leading == req.whereConds[0].col) {
                            idxName = info.indexName; break;
                        }
                    }
                }
            }

            if (useIndex) {
                std::string det = "WHERE " + req.whereConds[0].col +
                                  " = " + req.whereConds[0].val;
                addStep("INDEX", req.tableName, det, idxName);
                // Weitere WHERE-Bedingungen als FILTER
                if (req.whereConds.size() > 1) {
                    std::string fdet;
                    for (size_t i = 1; i < req.whereConds.size(); ++i) {
                        if (i > 1) fdet += " " + req.whereLogic + " ";
                        fdet += req.whereConds[i].col + " " +
                                req.whereConds[i].op  + " " + req.whereConds[i].val;
                    }
                    addStep("FILTER", "-", fdet, "-");
                }
            } else {
                addStep("SCAN", req.tableName, "FULL SCAN", "-");
                if (!req.whereConds.empty()) {
                    std::string det;
                    for (size_t i = 0; i < req.whereConds.size(); ++i) {
                        if (i > 0) det += " " + req.whereLogic + " ";
                        det += req.whereConds[i].col + " " +
                               req.whereConds[i].op  + " " + req.whereConds[i].val;
                    }
                    addStep("FILTER", "-", det, "-");
                }
            }
        }

        // GROUP BY
        if (req.isGroupBy) {
            std::string det = "BY ";
            for (size_t i = 0; i < req.groupByCols.size(); ++i) {
                if (i > 0) det += ", ";
                det += req.groupByCols[i];
            }
            addStep("GROUP", "-", det, "-");

            // HAVING
            if (!req.havingConds.empty()) {
                const auto& hv = req.havingConds[0];
                addStep("FILTER", "-",
                        "HAVING " + hv.aggFunc + "(" + hv.aggCol + ") " +
                        hv.op + " " + hv.val, "-");
            }

            // AGGREGATE aus selectItems
            for (const auto& si : req.selectItems) {
                if (si.isAgg) {
                    addStep("AGGREGATE", "-",
                            si.aggFunc + "(" + si.aggCol + ")", "-");
                    break;
                }
            }
        }

        // Einfaches AGGREGATE (ohne GROUP BY)
        if (req.isAggregate) {
            addStep("AGGREGATE", "-", req.aggFunc + "(" + req.aggCol + ")", "-");
        }

        // SORT (Phase 38: multi-column)
        if (!req.orderByCols.empty()) {
            std::string sortDetail;
            for (size_t i = 0; i < req.orderByCols.size(); ++i) {
                if (i > 0) sortDetail += ", ";
                sortDetail += req.orderByCols[i].first;
                sortDetail += req.orderByCols[i].second ? " DESC" : " ASC";
            }
            addStep("SORT", "-", sortDetail, "-");
        }

        // LIMIT / OFFSET (Phase 38)
        if (req.limit >= 0) {
            std::string limDetail = std::to_string(req.limit);
            if (req.limitOffset > 0) limDetail += " OFFSET " + std::to_string(req.limitOffset);
            addStep("LIMIT", "-", limDetail, "-");
        }

        // PROJECT — Spalten aus selectItems, selectColumns oder "*"
        std::string projDet;
        if (!req.selectItems.empty()) {
            for (size_t i = 0; i < req.selectItems.size(); ++i) {
                if (i > 0) projDet += ", ";
                const auto& si = req.selectItems[i];
                if      (!si.alias.empty())   projDet += si.alias;
                else if (!si.colName.empty()) projDet += si.colName;
                else if (si.isAgg)            projDet += si.aggFunc + "(" + si.aggCol + ")";
                else if (si.isFuncExpr)       projDet += si.funcName + "(...)";
            }
        } else if (!req.selectColumns.empty()) {
            for (size_t i = 0; i < req.selectColumns.size(); ++i) {
                if (i > 0) projDet += ", ";
                projDet += req.selectColumns[i];
            }
        } else {
            projDet = "* (alle Spalten)";
        }
        addStep("PROJECT", "-", projDet, "-");

        return plan;
    }

    // ── Phase 48: EXPLAIN mit Optimizer-Notizen ───────────────
    // Optimizer-Notiz-Struct (forward-deklariert hier, da optimizer.hpp Engine inkludiert)
    struct OptimizerNote {
        std::string step;
        std::string original;
        std::string optimized;
        double costBefore = 0.0;
        double costAfter  = 0.0;
    };

    ExplainPlan buildExplainWithNotes(const ExplainRequest& req,
                                      const std::vector<OptimizerNote>& notes) const {
        ExplainPlan plan = buildExplain(req);
        int nr = static_cast<int>(plan.steps.size()) + 1;
        for (const auto& note : notes) {
            std::string det = note.original + " => " + note.optimized;
            plan.steps.push_back({nr++, "OPT:" + note.step, "-", det, "-"});
        }
        return plan;
    }

    // ── Phase 48: Row count accessor for optimizer ────────────
    size_t getRowCount(const std::string& tableName) const {
        auto it = tables_.find(tableName);
        if (it == tables_.end()) return 0;
        return it->second.rowCount();
    }

    std::map<std::string, Table>  tables_;
    std::map<std::string, std::string> views_;   // Phase 24: name → SQL
    std::set<std::string> tempTableNames_;        // Phase 41: CTE-Tabellennamen

    // Transaktionszustand
    bool                     inTransaction_ = false;
    std::string              walPath_;
    std::vector<BufferedOp>  txBuffer_;

    // Phase 64: SAVEPOINT stack
    struct SavepointEntry {
        std::string name;
        std::size_t txSize;  // txBuffer_.size() when savepoint was created
    };
    std::vector<SavepointEntry> savepointStack_;

    // Phase 71: MVCC
    TransactionManager txManager_;
    uint64_t           mvccTxId_      = 0;  // current tx's MVCC ID (0 = none)
    std::string        isolationLevel_ = "REPEATABLE READ";

    // Phase 46: User management
    std::map<std::string, UserDef> users_;
    std::string currentUser_ = "root";

    // Phase 75: Row-Level Security
    std::map<std::string, std::vector<RlsPolicy>> rlsPolicies_;
    std::set<std::string> rlsEnabled_;

    // Phase 77: Parallel Query Execution
    ParallelExecutor parallelExec_;

    // Phase 78: Table Inheritance
    std::map<std::string, std::string>              tableParent_;    // child key → parent key
    std::map<std::string, std::vector<std::string>> tableChildren_;  // parent key → child keys

    // Phase 80: Column Store Engine
    std::map<std::string, ColumnTable> columnTables_;

    // Phase 84: Page-based I/O
    PageManager pagedManager_;
    std::map<std::string, PagedTable> pagedTables_;

    // Phase 81: Logical Replication
    std::map<std::string, PublicationDef> publications_;
    std::map<std::string, SubscriptionDef> subscriptions_;
    LogicalReplLog logicalLog_;

    // Phase 89: Foreign Data Wrapper
    std::map<std::string, ServerDef>       servers_;
    std::map<std::string, ForeignTableDef> foreignTables_;

    // Phase 85: WAL Checkpointing + Auto-Vacuum
    CheckpointManager checkpointMgr_;
    VacuumManager     vacuumMgr_;

public:
    // ── Phase 55: Öffentliche Hilfsmethoden ─────────────────────────────────────

    // Phase 63: public static helper — check if name refers to INFORMATION_SCHEMA
    static bool isInfoSchemaName(const std::string& name) {
        if (name.size() < 18) return false;
        std::string low = name;
        for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return low.substr(0, 18) == "information_schema" &&
               (low.size() == 18 || low[18] == '.');
    }

    // Gibt die Spalten einer Tabelle zurück (für INSERT-Umordnung in dispatch)
    const std::vector<Column>& tableColumns(const std::string& tblRaw) const {
        auto name = resolveTableName(tblRaw);
        return getTable(name).columns();
    }

    // Öffentlicher Zugriff auf evaluateFunc (für SELECT ohne FROM) ──
    // Erstellt eine temporäre leere Tabelle + leere Zeile und ruft evaluateFunc auf.
    std::string evalFuncPublic(const std::string& fn,
                               const std::vector<std::string>& args) {
        ExtMgrGuard guard(&extensionMgr_);
        Table tmpTbl("", {Column("_", "TEXT")});
        Row  emptyRow(std::vector<std::string>{"NULL"});
        return evaluateFunc(fn, args, tmpTbl, emptyRow);
    }

    // ── Phase 90: Extension Manager ──────────────────────────
    ExtensionManager& getExtensionManager() { return extensionMgr_; }
    const ExtensionManager& getExtensionManager() const { return extensionMgr_; }

    // ── Phase 97: Time-Series Manager ────────────────────────
    TimeSeriesManager& getTimeSeriesManager() { return tsManager_; }
    const TimeSeriesManager& getTimeSeriesManager() const { return tsManager_; }

    // ── Phase 105: Query Federation ───────────────────────────
    FederationManager& getFederationManager() { return fedMgr_; }
    const FederationManager& getFederationManager() const { return fedMgr_; }

    bool isFederatedTable(const std::string& name) const {
        return fedMgr_.isFederatedTable(name);
    }

    // Execute a federated SELECT and return a plain Table
    Table selectFederated(const std::string& name,
                          const std::string& where = "",
                          const std::vector<std::string>& cols = {}) {
        FedTable ft = fedMgr_.executeFederated(name, where, cols);
        std::vector<Column> tCols;
        for (const auto& fc : ft.columns)
            tCols.push_back(Column(fc.name, fc.type));
        Table t(ft.name, std::move(tCols));
        for (const auto& fr : ft.rows) {
            Row r(fr.values);
            // pad to column count
            while (r.values.size() < t.columns().size())
                r.values.push_back("NULL");
            t.mutableRows().push_back(std::move(r));
        }
        return t;
    }

    // RAII guard: sets/restores the thread-local extension manager pointer
    struct ExtMgrGuard {
        ExtensionManager* prev_;
        explicit ExtMgrGuard(ExtensionManager* em) {
            prev_ = Engine::tl_extMgr();
            Engine::tl_extMgr() = em;
        }
        ~ExtMgrGuard() { Engine::tl_extMgr() = prev_; }
    };

    // Sets TL pointer for the duration of this call (used internally before static calls)
    void setExtMgrForThread() { tl_extMgr() = &extensionMgr_; }

    // ── Phase 54A: Query Cache ────────────────────────────────
    QueryCache& getQueryCache() { return queryCache_; }
    const QueryCache& getQueryCache() const { return queryCache_; }

    void invalidateCache(const std::string& tableName) {
        // Invalidate both raw name (as stored by parser) and resolved key
        queryCache_.invalidate(tableName);
        auto key = resolveTableName(tableName);
        queryCache_.invalidate(key);
        // Phase 78: also invalidate ancestor tables (SELECT FROM parent includes child rows)
        auto it = tableParent_.find(key);
        while (it != tableParent_.end()) {
            const auto& pk = it->second;
            queryCache_.invalidate(pk);
            // also invalidate without schema prefix (raw parser name)
            auto dot = pk.rfind('.');
            if (dot != std::string::npos)
                queryCache_.invalidate(pk.substr(dot + 1));
            it = tableParent_.find(pk);
        }
    }

    // ── Phase 96: Compression management ─────────────────────────

    void setTableCompression(const std::string& tblRaw, CompressionType ct) {
        auto name = resolveTableName(tblRaw);
        auto it = tables_.find(name);
        if (it == tables_.end())
            throw std::runtime_error("Table not found: " + tblRaw);
        it->second.compressionType = ct;
    }

    std::string showCompressionStats(const std::string& tblRaw) const {
        auto name = resolveTableName(tblRaw);
        const Table& tbl = getTable(name);

        CompressionType ct = tbl.compressionType;
        std::string typeName = compressionTypeName(ct);

        // Collect all row values
        std::vector<std::string> allValues;
        size_t rawSize = 0;
        for (const auto& row : tbl.rows()) {
            if (row.xmax != 0) continue;
            for (const auto& v : row.values) {
                allValues.push_back(v);
                rawSize += v.size();
            }
        }

        size_t compressedSize = 0;

        if (ct == CompressionType::NONE) {
            compressedSize = rawSize;
        } else if (ct == CompressionType::RLE) {
            RleCompressor rle;
            // Build a single blob of all values
            std::string blob;
            blob.reserve(rawSize);
            for (const auto& v : allValues) blob += v;
            auto compressed = rle.compress(blob);
            compressedSize = compressed.size();
        } else if (ct == CompressionType::LZ4) {
            Lz4Compressor lz4;
            std::string blob;
            blob.reserve(rawSize);
            for (const auto& v : allValues) blob += v;
            auto compressed = lz4.compress(blob);
            compressedSize = compressed.size();
        } else if (ct == CompressionType::DICTIONARY) {
            // Build dictionary from current values, then compress each
            DictCompressor dict;
            const_cast<Table&>(tbl).dictCompressor.buildDictionary(allValues);
            std::vector<uint8_t> allCompressed;
            for (const auto& v : allValues) {
                auto tok = const_cast<Table&>(tbl).dictCompressor.compress(v);
                allCompressed.insert(allCompressed.end(), tok.begin(), tok.end());
            }
            compressedSize = allCompressed.size();
        } else if (ct == CompressionType::ZSTD) {
            ZstdCompressor zstd;
            std::string blob;
            blob.reserve(rawSize);
            for (const auto& v : allValues) blob += v;
            auto compressed = zstd.compress(blob);
            compressedSize = compressed.size();
        }

        double ratio = (rawSize > 0)
            ? (static_cast<double>(rawSize) / static_cast<double>(compressedSize))
            : 1.0;
        double savings = (rawSize > 0 && compressedSize < rawSize)
            ? (1.0 - static_cast<double>(compressedSize) / static_cast<double>(rawSize)) * 100.0
            : 0.0;

        std::ostringstream oss;
        oss << "  Compression Stats for table '" << tblRaw << "':\n";
        oss << "  ┌─────────────────────────────────────────┐\n";
        oss << "  │ Algorithm   : " << typeName << "\n";
        oss << "  │ Raw size    : " << rawSize << " bytes\n";
        oss << "  │ Compressed  : " << compressedSize << " bytes\n";
        oss << std::fixed << std::setprecision(2);
        oss << "  │ Ratio       : " << ratio << "x\n";
        oss << "  │ Space saved : " << savings << "%\n";
        oss << "  │ Row count   : " << allValues.size() / (tbl.columns().size() > 0 ? tbl.columns().size() : 1) << "\n";
        oss << "  └─────────────────────────────────────────┘\n";
        return oss.str();
    }

    // ── Phase 62: Partition management ───────────────────────────
    void setTablePartitionInfo(const std::string& tblRaw, const PartitionInfo& pi) {
        auto name = resolveTableName(tblRaw);
        auto it = tables_.find(name);
        if (it == tables_.end()) throw std::runtime_error("Table not found: " + tblRaw);
        it->second.setPartitionInfo(pi);
    }

    const PartitionInfo& getTablePartitionInfo(const std::string& tblRaw) const {
        auto name = resolveTableName(tblRaw);
        return getTable(name).getPartitionInfo();
    }

    // SHOW PARTITIONS FROM tbl — returns printable lines
    std::vector<std::string> showPartitions(const std::string& tblRaw) const {
        auto name = resolveTableName(tblRaw);
        const Table& tbl = getTable(name);
        const PartitionInfo& pi = tbl.getPartitionInfo();
        if (!pi.hasPartitions()) {
            return {"Table '" + tblRaw + "' has no partitions."};
        }
        auto stats = tbl.getPartitionStats();
        std::vector<std::string> lines;
        std::string typeStr;
        if      (pi.type == PartitionType::RANGE) typeStr = "RANGE";
        else if (pi.type == PartitionType::LIST)  typeStr = "LIST";
        else if (pi.type == PartitionType::HASH)  typeStr = "HASH";
        lines.push_back("Table: " + tblRaw + "  PARTITION BY " + typeStr + "(" + pi.column + ")");
        lines.push_back(std::string(60, '-'));
        lines.push_back("Name                 Type    Description              Rows");
        lines.push_back(std::string(60, '-'));
        for (auto& [pname, cnt] : stats) {
            std::string desc;
            if (pi.type == PartitionType::RANGE) {
                for (auto& r : pi.ranges) {
                    if (r.name == pname) { desc = "VALUES LESS THAN (" + r.limitStr + ")"; break; }
                }
            } else if (pi.type == PartitionType::LIST) {
                for (auto& l : pi.lists) {
                    if (l.name == pname) {
                        desc = "VALUES IN (";
                        for (size_t i = 0; i < l.values.size(); ++i) {
                            if (i) desc += ",";
                            desc += l.values[i];
                        }
                        desc += ")";
                        break;
                    }
                }
            } else if (pi.type == PartitionType::HASH) {
                desc = "HASH bucket";
            }
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%-20s %-7s %-24s %zu",
                          pname.c_str(), typeStr.c_str(), desc.c_str(), cnt);
            lines.push_back(buf);
        }
        return lines;
    }

    // ALTER TABLE t ADD PARTITION for RANGE
    void addRangePartition(const std::string& tblRaw, const PartitionRangeDef& def) {
        auto name = resolveTableName(tblRaw);
        auto it = tables_.find(name);
        if (it == tables_.end()) throw std::runtime_error("Table not found: " + tblRaw);
        PartitionInfo pi = it->second.getPartitionInfo();
        if (pi.type != PartitionType::RANGE)
            throw std::runtime_error("Table is not RANGE-partitioned");
        pi.ranges.push_back(def);
        it->second.setPartitionInfo(pi);
    }

    // ALTER TABLE t ADD PARTITION for LIST
    void addListPartition(const std::string& tblRaw, const PartitionListDef& def) {
        auto name = resolveTableName(tblRaw);
        auto it = tables_.find(name);
        if (it == tables_.end()) throw std::runtime_error("Table not found: " + tblRaw);
        PartitionInfo pi = it->second.getPartitionInfo();
        if (pi.type != PartitionType::LIST)
            throw std::runtime_error("Table is not LIST-partitioned");
        pi.lists.push_back(def);
        it->second.setPartitionInfo(pi);
    }

    // ALTER TABLE t DROP PARTITION name
    void dropPartitionByName(const std::string& tblRaw, const std::string& partName) {
        auto name = resolveTableName(tblRaw);
        auto it = tables_.find(name);
        if (it == tables_.end()) throw std::runtime_error("Table not found: " + tblRaw);
        PartitionInfo pi = it->second.getPartitionInfo();
        bool found = false;
        if (pi.type == PartitionType::RANGE) {
            for (auto rit = pi.ranges.begin(); rit != pi.ranges.end(); ++rit) {
                if (rit->name == partName) { pi.ranges.erase(rit); found = true; break; }
            }
        } else if (pi.type == PartitionType::LIST) {
            for (auto lit = pi.lists.begin(); lit != pi.lists.end(); ++lit) {
                if (lit->name == partName) { pi.lists.erase(lit); found = true; break; }
            }
        }
        if (!found) throw std::runtime_error("Partition not found: " + partName);
        it->second.setPartitionInfo(pi);
    }

    // Returns pruned partition names for a WHERE condition
    std::vector<std::string> prunePartitions(const std::string& tblRaw,
                                              const std::string& col,
                                              const std::string& op,
                                              const std::string& val) const {
        auto name = resolveTableName(tblRaw);
        return getTable(name).prunePartitions(col, op, val);
    }

    // ── Phase 72: Public op-apply for WAL recovery ───────────────
    void applyBufferedOp(const BufferedOp& op) { applyOp(op); }

    // ── Phase 72: Recovery status ─────────────────────────────────
    struct RecoveryStatus {
        bool hadWal          = false;
        int  recoveredTxCount = 0;
        int  discardedTxCount = 0;
        int  replayedOpCount  = 0;
    };

    void setRecoveryStatus(bool hadWal, int recovered, int discarded, int ops) {
        recoveryStatus_ = {hadWal, recovered, discarded, ops};
    }

    void showRecoveryStatus() const {
        std::cout << "\n";
        if (!recoveryStatus_.hadWal) {
            std::cout << "  Kein WAL bei Startup gefunden — kein Recovery nötig.\n\n";
            return;
        }
        std::cout << "  WAL Recovery Status:\n";
        std::cout << "  ┌──────────────────────────────────────┐\n";
        std::cout << "  │ WAL gefunden beim Start    JA         │\n";
        std::cout << "  │ Committete Transaktionen   "
                  << recoveryStatus_.recoveredTxCount;
        for (int sp = static_cast<int>(std::to_string(recoveryStatus_.recoveredTxCount).size());
             sp < 10; ++sp) std::cout << " ";
        std::cout << "│\n";
        std::cout << "  │ Verworfen (kein COMMIT)    "
                  << recoveryStatus_.discardedTxCount;
        for (int sp = static_cast<int>(std::to_string(recoveryStatus_.discardedTxCount).size());
             sp < 10; ++sp) std::cout << " ";
        std::cout << "│\n";
        std::cout << "  │ Wiederhergestellte Ops     "
                  << recoveryStatus_.replayedOpCount;
        for (int sp = static_cast<int>(std::to_string(recoveryStatus_.replayedOpCount).size());
             sp < 10; ++sp) std::cout << " ";
        std::cout << "│\n";
        std::cout << "  └──────────────────────────────────────┘\n\n";
    }

    // ── Phase 72: Materialized Views ─────────────────────────────

    struct MaterializedViewDef {
        std::string          name;
        std::string          sql;
        std::string          lastRefresh;   // "YYYY-MM-DD HH:MM:SS"
        std::vector<Column>  columns;
        std::vector<Row>     data;
    };

    void createMaterializedView(const std::string& name,
                                const std::string& sql,
                                const std::vector<Column>& cols,
                                const std::vector<Row>& data) {
        if (materializedViews_.count(name))
            throw std::runtime_error("Materialized View '" + name + "' existiert bereits.");
        MaterializedViewDef mv;
        mv.name        = name;
        mv.sql         = sql;
        mv.lastRefresh = currentTimestamp();
        mv.columns     = cols;
        mv.data        = data;
        materializedViews_[name] = std::move(mv);
    }

    void setMaterializedViewData(const std::string& name,
                                  const std::vector<Column>& cols,
                                  const std::vector<Row>& data) {
        auto it = materializedViews_.find(name);
        if (it == materializedViews_.end())
            throw std::runtime_error("Materialized View '" + name + "' nicht gefunden.");
        it->second.columns     = cols;
        it->second.data        = data;
        it->second.lastRefresh = currentTimestamp();
    }

    // Create with empty data (used on load before refresh)
    void createMaterializedViewEmpty(const std::string& name, const std::string& sql) {
        if (materializedViews_.count(name)) return;  // already loaded
        MaterializedViewDef mv;
        mv.name = name;
        mv.sql  = sql;
        materializedViews_[name] = std::move(mv);
    }

    void dropMaterializedView(const std::string& name) {
        if (!materializedViews_.erase(name))
            throw std::runtime_error("Materialized View '" + name + "' nicht gefunden.");
    }

    bool isMaterializedView(const std::string& nameRaw) const {
        auto key = resolveTableName(nameRaw);
        return materializedViews_.count(key) > 0 ||
               materializedViews_.count(nameRaw) > 0;
    }

    const MaterializedViewDef& getMaterializedView(const std::string& nameRaw) const {
        // Try exact name first, then resolved
        auto it = materializedViews_.find(nameRaw);
        if (it != materializedViews_.end()) return it->second;
        auto key = resolveTableName(nameRaw);
        auto it2 = materializedViews_.find(key);
        if (it2 != materializedViews_.end()) return it2->second;
        throw std::runtime_error("Materialized View '" + nameRaw + "' nicht gefunden.");
    }

    const std::map<std::string, MaterializedViewDef>& getAllMaterializedViews() const {
        return materializedViews_;
    }

    void showMaterializedViews() const {
        std::cout << "\n";
        if (materializedViews_.empty()) {
            std::cout << "  (Keine Materialized Views vorhanden)\n\n";
            return;
        }
        // Column widths
        size_t wName = 4, wSql = 3, wRef = 19;
        for (const auto& [n, mv] : materializedViews_) {
            wName = std::max(wName, n.size());
            wSql  = std::max(wSql,  mv.sql.size() > 40 ? (size_t)40 : mv.sql.size());
            wRef  = std::max(wRef,  mv.lastRefresh.size());
        }
        auto hline = [&](const char* l, const char* m, const char* r, const char* f) {
            std::cout << l;
            for (size_t j = 0; j < wName + 2; ++j) std::cout << f;
            std::cout << m;
            for (size_t j = 0; j < wRef + 2;  ++j) std::cout << f;
            std::cout << m;
            for (size_t j = 0; j < 6;          ++j) std::cout << f;  // Rows
            std::cout << m;
            for (size_t j = 0; j < wSql + 2;   ++j) std::cout << f;
            std::cout << r << "\n";
        };
        auto cell = [](const std::string& s, size_t w) {
            std::cout << " " << s;
            for (size_t j = s.size(); j < w; ++j) std::cout << " ";
            std::cout << " \u2502";
        };

        hline("\u250c", "\u252c", "\u2510", "\u2500");
        std::cout << "\u2502";
        cell("Name",         wName);
        cell("Zuletzt aktual.", wRef);
        cell("Rows", 4);
        cell("SQL",          wSql);
        std::cout << "\n";
        hline("\u251c", "\u253c", "\u2524", "\u2500");

        for (const auto& [n, mv] : materializedViews_) {
            std::string sqlShort = mv.sql.size() > 40 ? mv.sql.substr(0, 37) + "..." : mv.sql;
            std::string rowsStr  = std::to_string(mv.data.size());
            std::cout << "\u2502";
            cell(n,               wName);
            cell(mv.lastRefresh,  wRef);
            cell(rowsStr,         4);
            cell(sqlShort,        wSql);
            std::cout << "\n";
        }

        hline("\u2514", "\u2534", "\u2518", "\u2500");
        std::cout << "  " << materializedViews_.size() << " Materialized View(s)\n\n";
    }

    // ── Phase 71: MVCC public API ─────────────────────────────

    // VACUUM: physically remove logically-deleted rows from all tables
    size_t vacuumAll() {
        size_t total = 0;
        for (auto& [name, tbl] : tables_) {
            total += tbl.vacuum([this](uint64_t txId) {
                return txManager_.isCommitted(txId);
            });
        }
        return total;
    }

    // VACUUM on a single table
    size_t vacuumTable(const std::string& tblRaw) {
        auto name = resolveTableName(tblRaw);
        return getTable(name).vacuum([this](uint64_t txId) {
            return txManager_.isCommitted(txId);
        });
    }

    // Count dead rows across all tables (for diagnostics)
    size_t deadRowCount() const {
        size_t total = 0;
        for (const auto& [name, tbl] : tables_)
            total += tbl.deadRowCount();
        return total;
    }

    // ── Phase 85: VACUUM with VacuumManager integration ──────
    size_t vacuumAllTracked() {
        size_t total = 0;
        for (auto& [name, tbl] : tables_) {
            size_t cleaned = tbl.vacuum([this](uint64_t txId) {
                return txManager_.isCommitted(txId);
            });
            if (cleaned) {
                total += cleaned;
                vacuumMgr_.resetDeadTuples(name);
            }
        }
        return total;
    }

    size_t vacuumTableTracked(const std::string& tblRaw) {
        auto name = resolveTableName(tblRaw);
        size_t cleaned = getTable(name).vacuum([this](uint64_t txId) {
            return txManager_.isCommitted(txId);
        });
        if (cleaned) vacuumMgr_.resetDeadTuples(name);
        return cleaned;
    }

    // ── Phase 85: Checkpoint API ──────────────────────────────
    void doCheckpoint() {
        uint64_t txId = txManager_.currentGlobalId();
        checkpointMgr_.doCheckpoint(txId);
    }

    bool shouldAutoCheckpoint() const {
        return checkpointMgr_.shouldAutoCheckpoint();
    }

    void setAutoCheckpointInterval(uint64_t n) {
        checkpointMgr_.setAutoCheckpointInterval(n);
    }

    void showCheckpointStatus() const {
        checkpointMgr_.showStatus();
    }

    // ── Phase 85: Vacuum Manager API ─────────────────────────
    void showVacuumStatus() const {
        // sync tracked dead tuples with actual table state
        vacuumMgr_.showStatus();
    }

    void setAutoVacuumEnabled(bool on) {
        vacuumMgr_.setAutoVacuumEnabled(on);
    }

    void setAutoVacuumThreshold(size_t n) {
        vacuumMgr_.setAutoVacuumThreshold(n);
    }

    // Called from main.cpp to start background thread
    void startAutoVacuum() {
        vacuumMgr_.startAutoVacuum([this]() {
            return vacuumAllTracked();
        });
    }

    void stopAutoVacuum() {
        vacuumMgr_.stopAutoVacuum();
    }

    // SHOW TRANSACTIONS
    void showTransactions() const {
        txManager_.showTransactions();
    }

    // SET TRANSACTION ISOLATION LEVEL
    void setIsolationLevel(const std::string& level) {
        isolationLevel_ = level;
    }

    const std::string& getIsolationLevel() const { return isolationLevel_; }

    // ── Phase 73: Buffer Pool Manager API ────────────────────────

    void setBufferPoolSize(int mb) {
        bufferPool_.setSizeMB(mb);
    }

    int getBufferPoolSizeMB() const {
        return bufferPool_.getSizeMB();
    }

    void showBufferPoolStatus() const {
        bufferPool_.showStatus();
    }

    // Flush all dirty pages via persist callback
    void flushBufferPool(const std::function<void(const std::string&)>& flushFn) {
        bufferPool_.flushAll(flushFn);
    }

    // Get dirty page list (for background thread)
    std::vector<std::string> getDirtyBufferPages() const {
        return bufferPool_.getDirtyPages();
    }

    // Mark a page clean (called after successful save)
    void markBufferPageClean(const std::string& tableName) {
        bufferPool_.markClean(tableName);
    }

    // Phase 112: RwLock stats for SHOW ENGINE STATUS
    struct RwLockInfo {
        std::string tableName;
        int         activeReaders;
        bool        hasWriter;
    };
    std::vector<RwLockInfo> getRwLockStats() const {
        std::lock_guard<std::mutex> g(rwLockMapMu_);
        std::vector<RwLockInfo> out;
        out.reserve(rwLocks_.size());
        for (const auto& [name, lk] : rwLocks_)
            out.push_back({name, lk->activeReaders(), lk->hasActiveWriter()});
        return out;
    }

private:
    // Phase 90: Extension Manager
    ExtensionManager extensionMgr_;

    // Phase 54A: Query Cache instance
    QueryCache queryCache_;

    // Phase 63: INFORMATION_SCHEMA virtual table cache (mutable for const access)
    mutable std::map<std::string, Table> infoSchemaCache_;

    // Phase 72: Materialized Views
    std::map<std::string, MaterializedViewDef> materializedViews_;

    // Phase 73: Buffer Pool (mutable so const methods like selectWhere can track hits)
    mutable BufferPool bufferPool_;

    // Phase 72: Recovery status (set by main.cpp after startup recovery)
    RecoveryStatus recoveryStatus_;

    // Phase 97: Time-Series Manager
    TimeSeriesManager tsManager_;

    // Phase 98: Change Data Capture
    CdcManager cdcMgr_;

    // Phase 105: Query Federation
    FederationManager fedMgr_;

    // Phase 113: DP planner result for last JOIN query (for EXPLAIN)
    mutable JoinPlan lastJoinPlan_;

    // Phase 112: per-table RwLocks for concurrent read optimization
    // unique_ptr because RwLock (shared_mutex) is non-movable/non-copyable
    mutable std::mutex                                      rwLockMapMu_;
    mutable std::map<std::string, std::unique_ptr<RwLock>> rwLocks_;

    // Get-or-create the RwLock for a table (thread-safe)
    RwLock& getOrCreateRwLock(const std::string& tblKey) const {
        std::lock_guard<std::mutex> g(rwLockMapMu_);
        auto it = rwLocks_.find(tblKey);
        if (it != rwLocks_.end()) return *it->second;
        auto res = rwLocks_.emplace(tblKey, std::make_unique<RwLock>());
        return *res.first->second;
    }

    // Thread-local set of tables whose lock is already held on this thread.
    // Prevents recursive deadlock when triggers call DML on the same table.
    static std::set<std::string>& threadHeldLocks() {
        static thread_local std::set<std::string> s;
        return s;
    }

    // RAII scopes that skip acquisition if already held (recursion guard)
    struct WriteScope {
        RwLock* lk_; std::string key_; bool owned_;
        WriteScope(RwLock& lk, const std::string& key)
            : lk_(&lk), key_(key), owned_(!threadHeldLocks().count(key)) {
            if (owned_) {
                threadHeldLocks().insert(key_);
                lk_->writeLock();
                g_atomicStats().incWrite();
            } else {
                g_atomicStats().incSkip();
            }
        }
        ~WriteScope() {
            if (owned_) { lk_->writeUnlock(); threadHeldLocks().erase(key_); }
        }
        WriteScope(const WriteScope&) = delete;
        WriteScope& operator=(const WriteScope&) = delete;
    };

    struct ReadScope {
        RwLock* lk_; std::string key_; bool owned_;
        ReadScope(RwLock& lk, const std::string& key)
            : lk_(&lk), key_(key), owned_(!threadHeldLocks().count(key)) {
            if (owned_) {
                threadHeldLocks().insert(key_);
                lk_->readLock();
                g_atomicStats().incRead();
            } else {
                g_atomicStats().incSkip();
            }
        }
        ~ReadScope() {
            if (owned_) { lk_->readUnlock(); threadHeldLocks().erase(key_); }
        }
        ReadScope(const ReadScope&) = delete;
        ReadScope& operator=(const ReadScope&) = delete;
    };

    // Phase 72: timestamp helper
    static std::string currentTimestamp() {
        std::time_t t = std::time(nullptr);
        char buf[20] = {};
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        return buf;
    }

    // Phase 75: RLS private helpers
    static std::vector<std::string> splitPipe_(const std::string& s) {
        std::vector<std::string> parts;
        std::stringstream ss(s);
        std::string tok;
        while (std::getline(ss, tok, '|')) parts.push_back(tok);
        return parts;
    }

    bool evaluateRlsExpr_(const std::string& expr, const Row& row,
                          const std::vector<std::string>& colNames) const {
        if (expr == "1 = 1" || expr == "1=1") return true;
        if (expr == "1 = 0" || expr == "1=0") return false;

        std::string e = expr;
        size_t p;
        while ((p = e.find("CURRENT_USER_ID()")) != std::string::npos)
            e.replace(p, 17, currentUser_);

        auto eqPos = e.find('=');
        if (eqPos == std::string::npos) return true;
        std::string lhs = e.substr(0, eqPos);
        std::string rhs = e.substr(eqPos + 1);
        auto trim = [](std::string& s) {
            while (!s.empty() && s.front() == ' ') s.erase(s.begin());
            while (!s.empty() && s.back()  == ' ') s.pop_back();
        };
        trim(lhs); trim(rhs);
        if (rhs.size() >= 2 && rhs.front() == '\'' && rhs.back() == '\'')
            rhs = rhs.substr(1, rhs.size() - 2);

        for (size_t i = 0; i < colNames.size(); ++i) {
            if (colNames[i] == lhs) {
                if (i >= row.values.size()) return false;
                std::string cellVal = row.values[i];
                // Strip surrounding single quotes stored by the engine
                if (cellVal.size() >= 2 && cellVal.front() == '\'' && cellVal.back() == '\'')
                    cellVal = cellVal.substr(1, cellVal.size() - 2);
                return cellVal == rhs;
            }
        }
        return true;
    }

    // ── Phase 78: Inheritance helpers ────────────────────────────

    // Recursively collect rows from child tables, projected to parentColCount columns.
    // If allRows=true, collect all (no WHERE); otherwise apply conds/cis/logic.
    void collectInheritedRows_(const std::string& parentKey,
                               size_t parentColCount,
                               const std::vector<WhereCondition>& conds,
                               const std::vector<size_t>& cis,
                               const std::string& logic,
                               Table& result,
                               bool allRows) const {
        auto childIt = tableChildren_.find(parentKey);
        if (childIt == tableChildren_.end()) return;
        for (const auto& childKey : childIt->second) {
            auto cit = tables_.find(childKey);
            if (cit == tables_.end()) continue;
            for (const auto& row : cit->second.rows()) {
                if (row.xmax != 0) continue;
                // Project to parent column count
                std::vector<std::string> proj;
                proj.reserve(parentColCount);
                for (size_t i = 0; i < parentColCount && i < row.values.size(); ++i)
                    proj.push_back(row.values[i]);
                // Pad with NULL if child has fewer columns than parent (shouldn't happen)
                while (proj.size() < parentColCount) proj.push_back("NULL");
                Row projRow(proj);
                if (allRows || conds.empty() || rowMatchesByIdx(projRow, conds, cis, logic))
                    result.mutableRows().push_back(projRow); // bypass UNIQUE/PK checks
            }
            // Recurse into grandchildren
            collectInheritedRows_(childKey, parentColCount, conds, cis, logic, result, allRows);
        }
    }

    // Recursively count rows in child tables
    size_t countInheritedRows_(const std::string& parentKey) const {
        auto childIt = tableChildren_.find(parentKey);
        if (childIt == tableChildren_.end()) return 0;
        size_t n = 0;
        for (const auto& childKey : childIt->second) {
            auto cit = tables_.find(childKey);
            if (cit != tables_.end()) {
                for (const auto& row : cit->second.rows())
                    if (row.xmax == 0) ++n;
            }
            n += countInheritedRows_(childKey);
        }
        return n;
    }

    void printInheritanceTree_(const std::string& parent, int depth) const {
        auto childIt = tableChildren_.find(parent);
        if (childIt == tableChildren_.end()) return;
        for (const auto& child : childIt->second) {
            for (int i = 0; i < depth; ++i) std::cout << "  ";
            std::cout << "  → " << child << "\n";
            printInheritanceTree_(child, depth + 1);
        }
    }

    // ── Phase 77: Parallel filter ─────────────────────────────────
    std::vector<Row> parallelFilter_(
            const std::vector<Row>& rows,
            const std::vector<WhereCondition>& conds,
            const std::vector<size_t>& cis,
            const std::string& logic,
            int numWorkers) const {
        size_t total = rows.size();
        if (total == 0 || numWorkers <= 1) return {};
        size_t chunkSize = (total + static_cast<size_t>(numWorkers) - 1)
                           / static_cast<size_t>(numWorkers);
        std::vector<std::vector<Row>> partials(static_cast<size_t>(numWorkers));
        std::vector<std::thread> threads;
        threads.reserve(static_cast<size_t>(numWorkers));
        for (int w = 0; w < numWorkers; ++w) {
            size_t start = static_cast<size_t>(w) * chunkSize;
            if (start >= total) break;
            size_t end = std::min(start + chunkSize, total);
            threads.emplace_back([&, w, start, end]() {
                for (size_t i = start; i < end; ++i) {
                    const auto& row = rows[i];
                    if (row.xmax != 0) continue;
                    if (rowMatchesByIdx(row, conds, cis, logic))
                        partials[static_cast<size_t>(w)].push_back(row);
                }
            });
        }
        for (auto& t : threads) t.join();
        std::vector<Row> result;
        for (auto& p : partials)
            result.insert(result.end(), p.begin(), p.end());
        return result;
    }

    // ── Phase 77: Parallel aggregate ─────────────────────────────
    std::string parallelAggregate_(
            const Table& src,
            const std::string& func,
            size_t ci,
            int numWorkers) const {
        const auto& rows = src.rows();
        size_t total = rows.size();
        if (total == 0) return "NULL";
        size_t chunkSize = (total + static_cast<size_t>(numWorkers) - 1)
                           / static_cast<size_t>(numWorkers);
        if (func == "COUNT") {
            std::vector<size_t> counts(static_cast<size_t>(numWorkers), 0);
            std::vector<std::thread> threads;
            threads.reserve(static_cast<size_t>(numWorkers));
            for (int w = 0; w < numWorkers; ++w) {
                size_t start = static_cast<size_t>(w) * chunkSize;
                if (start >= total) break;
                size_t end = std::min(start + chunkSize, total);
                threads.emplace_back([&, w, start, end]() {
                    for (size_t i = start; i < end; ++i)
                        if (rows[i].xmax == 0) ++counts[static_cast<size_t>(w)];
                });
            }
            for (auto& t : threads) t.join();
            size_t n = 0; for (auto c : counts) n += c;
            return std::to_string(n);
        }
        std::vector<std::vector<double>> partials(static_cast<size_t>(numWorkers));
        std::vector<std::thread> threads;
        threads.reserve(static_cast<size_t>(numWorkers));
        for (int w = 0; w < numWorkers; ++w) {
            size_t start = static_cast<size_t>(w) * chunkSize;
            if (start >= total) break;
            size_t end = std::min(start + chunkSize, total);
            threads.emplace_back([&, w, start, end]() {
                for (size_t i = start; i < end; ++i) {
                    const auto& row = rows[i];
                    if (row.xmax != 0) continue;
                    if (ci < row.values.size()) {
                        try { partials[static_cast<size_t>(w)].push_back(
                            std::stod(row.values[ci])); }
                        catch (...) {}
                    }
                }
            });
        }
        for (auto& t : threads) t.join();
        std::vector<double> all;
        for (auto& p : partials) all.insert(all.end(), p.begin(), p.end());
        if (all.empty()) return "NULL";
        if (func == "MIN") return formatNum(*std::min_element(all.begin(), all.end()));
        if (func == "MAX") return formatNum(*std::max_element(all.begin(), all.end()));
        if (func == "SUM") { double s = 0; for (double v : all) s += v; return formatNum(s); }
        if (func == "AVG") { double s = 0; for (double v : all) s += v;
            return formatNum(s / static_cast<double>(all.size())); }
        return "NULL";
    }

    std::vector<Row> applyRls_(const std::string& table,
                               const std::vector<Row>& rows,
                               const std::string& cmd = "SELECT") const {
        if (!isRlsEnabled(table)) return rows;
        if (currentUser_ == "root" || currentUser_.empty()) return rows;

        auto pit = rlsPolicies_.find(table);
        if (pit == rlsPolicies_.end() || pit->second.empty()) return {};

        std::vector<std::string> colNames;
        try {
            const auto& cols = getTable(table).columns();
            colNames.reserve(cols.size());
            for (const auto& c : cols) colNames.push_back(c.name);
        } catch (...) {
            return {};
        }

        std::vector<Row> result;
        for (const auto& row : rows) {
            if (row.xmax != 0) continue;  // skip dead rows
            bool allowed = false;
            for (const auto& pol : pit->second) {
                if (pol.command != "ALL" && pol.command != cmd) continue;
                if (pol.role != "PUBLIC" && pol.role != currentUser_) continue;
                if (evaluateRlsExpr_(pol.usingExpr, row, colNames)) {
                    allowed = true;
                    break;
                }
            }
            if (allowed) result.push_back(row);
        }
        return result;
    }

    // ── Phase 81: Logical Replication private helpers ────────────

    void logicalLog_WriteIfPublished_(const std::string& tableName,
                                      const std::string& op,
                                      const std::vector<std::string>& cols,
                                      const std::vector<std::string>& vals) {
        for (const auto& [pn, pd] : publications_) {
            if (pd.allTables) {
                logicalLog_.writeChange(tableName, op, cols, vals);
                return;
            }
            for (const auto& t : pd.tables) {
                if (resolveTableName(t) == tableName || t == tableName) {
                    logicalLog_.writeChange(tableName, op, cols, vals);
                    return;
                }
            }
        }
    }

    void savePublications_() const {
        std::ofstream f("database.publications");
        for (const auto& [n, pd] : publications_) {
            f << n << "|" << (pd.allTables ? "*" : "");
            for (size_t i = 0; i < pd.tables.size(); ++i) {
                if (i) f << ",";
                f << pd.tables[i];
            }
            f << "\n";
        }
    }

    void loadPublications_() {
        std::ifstream f("database.publications");
        std::string line;
        while (std::getline(f, line)) {
            auto p = line.find('|');
            if (p == std::string::npos) continue;
            PublicationDef pd;
            pd.name = line.substr(0, p);
            std::string rest = line.substr(p + 1);
            if (rest == "*") { pd.allTables = true; }
            else if (!rest.empty()) {
                std::istringstream ss(rest);
                std::string tok;
                while (std::getline(ss, tok, ','))
                    if (!tok.empty()) pd.tables.push_back(tok);
            }
            publications_[pd.name] = std::move(pd);
        }
    }

    void saveSubscriptions_() const {
        std::ofstream f("database.subscriptions");
        for (const auto& [n, sd] : subscriptions_) {
            f << sd.name << "|" << sd.connection << "|" << sd.publication
              << "|" << (sd.enabled ? "1" : "0") << "\n";
        }
    }

    void loadSubscriptions_() {
        std::ifstream f("database.subscriptions");
        std::string line;
        while (std::getline(f, line)) {
            std::istringstream ss(line);
            std::string parts[4];
            for (int i = 0; i < 4 && std::getline(ss, parts[i], '|'); ++i) {}
            if (parts[0].empty()) continue;
            SubscriptionDef sd;
            sd.name = parts[0];
            sd.connection = parts[1];
            sd.publication = parts[2];
            sd.enabled = (parts[3] == "1");
            subscriptions_[sd.name] = std::move(sd);
        }
    }

    // ── Phase 84: Paged Table Schema Persistence ──────────────
    // Schema file format (one line per table):
    //   tableName|col1:TYPE1|col2:TYPE2|...
    void savePagedTableSchemas_() const {
        std::ofstream f("database.paged_schemas");
        for (const auto& [key, pt] : pagedTables_) {
            f << key;
            for (const auto& col : pt.columns())
                f << "|" << col.name << ":" << col.type;
            f << "\n";
        }
    }

    void loadPagedTableSchemas_() {
        std::ifstream f("database.paged_schemas");
        if (!f.is_open()) return;
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty()) continue;
            std::istringstream ss(line);
            std::string token;
            std::vector<std::string> parts;
            while (std::getline(ss, token, '|'))
                parts.push_back(token);
            if (parts.empty()) continue;
            std::string tblName = parts[0];
            std::vector<Column> cols;
            for (size_t i = 1; i < parts.size(); ++i) {
                auto colon = parts[i].find(':');
                if (colon == std::string::npos) continue;
                cols.emplace_back(parts[i].substr(0, colon),
                                  parts[i].substr(colon + 1));
            }
            if (!tblName.empty())
                pagedTables_.emplace(tblName, PagedTable(tblName, cols, pagedManager_));
        }
    }

public:
    // ── Phase 95: Public row matching helper ─────────────────────────────
    bool rowMatchesPublic(const Table& src,
                          const Row& row,
                          const std::vector<WhereCondition>& conds,
                          const std::string& logic) const {
        return rowMatches(src, row, conds, logic);
    }

    // ── Phase 95: pg_catalog + information_schema virtual tables ──────────

    bool isPgCatalogTable(const std::string& name) const {
        static const std::set<std::string> pgCatalogTables = {
            "pg_catalog.pg_tables",     "pg_tables",
            "pg_catalog.pg_class",      "pg_class",
            "pg_catalog.pg_attribute",  "pg_attribute",
            "pg_catalog.pg_index",      "pg_index",
            "pg_catalog.pg_user",       "pg_user",
            "pg_catalog.pg_database",   "pg_database",
            "pg_catalog.pg_namespace",  "pg_namespace",
            "pg_catalog.pg_type",       "pg_type",
            "pg_catalog.pg_proc",       "pg_proc",
            "information_schema.tables",
            "information_schema.columns",
        };
        std::string lower = name;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return pgCatalogTables.count(lower) > 0;
    }

    Table buildPgCatalogTable(const std::string& name) const {
        std::string lower = name;
        for (auto& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        // Helper: strip schema prefix from internal table name
        auto bareName = [](const std::string& n) -> std::string {
            auto dot = n.rfind('.');
            return dot != std::string::npos ? n.substr(dot + 1) : n;
        };

        // pg_catalog.pg_tables  — one row per user table
        if (lower == "pg_catalog.pg_tables" || lower == "pg_tables") {
            std::vector<Column> cols = {
                Column("schemaname", "TEXT"),
                Column("tablename",  "TEXT"),
                Column("tableowner", "TEXT"),
                Column("hasindexes", "TEXT"),
            };
            Table t("pg_tables", cols);
            for (const auto& [tname, tbl] : tables_) {
                bool hasIdx = !tbl.getIndexes().empty();
                t.insert(Row({"public", bareName(tname), "root", hasIdx ? "t" : "f"}));
            }
            return t;
        }

        // pg_catalog.pg_class  — tables + views + indexes
        if (lower == "pg_catalog.pg_class" || lower == "pg_class") {
            std::vector<Column> cols = {
                Column("relname",   "TEXT"),
                Column("relkind",   "TEXT"),
                Column("reltuples", "TEXT"),
            };
            Table t("pg_class", cols);
            for (const auto& [tname, tbl] : tables_) {
                t.insert(Row({bareName(tname), "r", std::to_string(tbl.rowCount())}));
                for (const auto& idx : tbl.getIndexes()) {
                    t.insert(Row({idx.indexName, "i", "0"}));
                }
            }
            for (const auto& [vname, _vsql] : views_) {
                t.insert(Row({vname, "v", "0"}));
            }
            return t;
        }

        // pg_catalog.pg_attribute  — one row per column per table
        if (lower == "pg_catalog.pg_attribute" || lower == "pg_attribute") {
            std::vector<Column> cols = {
                Column("attrelid",  "TEXT"),
                Column("attname",   "TEXT"),
                Column("atttypid",  "TEXT"),
                Column("attnum",    "TEXT"),
                Column("attnotnull","TEXT"),
            };
            Table t("pg_attribute", cols);
            for (const auto& [tname, tbl] : tables_) {
                std::string bname = bareName(tname);
                const auto& tcols = tbl.columns();
                for (size_t i = 0; i < tcols.size(); ++i) {
                    t.insert(Row({
                        bname,
                        tcols[i].name,
                        tcols[i].type,
                        std::to_string(i + 1),
                        tcols[i].notNull ? "t" : "f"
                    }));
                }
            }
            return t;
        }

        // pg_catalog.pg_index  — one row per index
        if (lower == "pg_catalog.pg_index" || lower == "pg_index") {
            std::vector<Column> cols = {
                Column("indrelid",    "TEXT"),
                Column("indexrelid",  "TEXT"),
                Column("indisunique", "TEXT"),
                Column("indisprimary","TEXT"),
            };
            Table t("pg_index", cols);
            for (const auto& [tname, tbl] : tables_) {
                std::string bname = bareName(tname);
                for (const auto& idx : tbl.getIndexes()) {
                    t.insert(Row({bname, idx.indexName, "f", "f"}));
                }
            }
            return t;
        }

        // pg_catalog.pg_user  — one row per user
        if (lower == "pg_catalog.pg_user" || lower == "pg_user") {
            std::vector<Column> cols = {
                Column("usename",     "TEXT"),
                Column("usecreatedb", "TEXT"),
                Column("usesysid",    "TEXT"),
            };
            Table t("pg_user", cols);
            int sysid = 1;
            for (const auto& [uname, _udef] : users_) {
                t.insert(Row({uname, "t", std::to_string(sysid++)}));
            }
            return t;
        }

        // pg_catalog.pg_database  — databases/schemas
        if (lower == "pg_catalog.pg_database" || lower == "pg_database") {
            std::vector<Column> cols = {
                Column("datname",  "TEXT"),
                Column("datdba",   "TEXT"),
                Column("encoding", "TEXT"),
            };
            Table t("pg_database", cols);
            t.insert(Row({"public", "root", "UTF8"}));
            for (const auto& s : schemas_) {
                if (s != "public") t.insert(Row({s, "root", "UTF8"}));
            }
            return t;
        }

        // pg_catalog.pg_namespace  — namespaces/schemas
        if (lower == "pg_catalog.pg_namespace" || lower == "pg_namespace") {
            std::vector<Column> cols = {
                Column("nspname",  "TEXT"),
                Column("nspowner", "TEXT"),
            };
            Table t("pg_namespace", cols);
            for (const auto& s : schemas_) {
                t.insert(Row({s, "root"}));
            }
            return t;
        }

        // pg_catalog.pg_type  — standard SQL types
        if (lower == "pg_catalog.pg_type" || lower == "pg_type") {
            std::vector<Column> cols = {
                Column("typname", "TEXT"),
                Column("typtype", "TEXT"),
                Column("typlen",  "TEXT"),
            };
            Table t("pg_type", cols);
            static const std::vector<std::array<std::string,3>> types = {
                {"int",   "b", "4"},
                {"int4",  "b", "4"},
                {"int8",  "b", "8"},
                {"int2",  "b", "2"},
                {"text",  "b", "-1"},
                {"real",  "b", "4"},
                {"float4","b", "4"},
                {"float8","b", "8"},
                {"bool",  "b", "1"},
                {"date",  "b", "4"},
                {"json",  "b", "-1"},
            };
            for (const auto& tp : types) {
                t.insert(Row({tp[0], tp[1], tp[2]}));
            }
            return t;
        }

        // pg_catalog.pg_proc  — built-in + extension functions
        if (lower == "pg_catalog.pg_proc" || lower == "pg_proc") {
            std::vector<Column> cols = {
                Column("proname",       "TEXT"),
                Column("pronamespace",  "TEXT"),
                Column("pronargs",      "TEXT"),
                Column("prorettype",    "TEXT"),
            };
            Table t("pg_proc", cols);
            static const std::vector<std::array<std::string,4>> builtins = {
                {"upper",   "pg_catalog", "1", "text"},
                {"lower",   "pg_catalog", "1", "text"},
                {"length",  "pg_catalog", "1", "int4"},
                {"concat",  "pg_catalog", "-1","text"},
                {"substr",  "pg_catalog", "3", "text"},
                {"trim",    "pg_catalog", "1", "text"},
                {"replace", "pg_catalog", "3", "text"},
                {"now",     "pg_catalog", "0", "text"},
                {"count",   "pg_catalog", "1", "int4"},
                {"sum",     "pg_catalog", "1", "text"},
                {"avg",     "pg_catalog", "1", "real"},
                {"min",     "pg_catalog", "1", "text"},
                {"max",     "pg_catalog", "1", "text"},
                {"coalesce","pg_catalog", "-1","text"},
                {"cast",    "pg_catalog", "2", "text"},
                {"abs",     "pg_catalog", "1", "real"},
            };
            for (const auto& f : builtins) {
                t.insert(Row({f[0], f[1], f[2], f[3]}));
            }
            // Add extension functions
            for (const auto& [pname, proc] : procedures_) {
                t.insert(Row({pname, "public",
                              std::to_string(proc.params.size()), "text"}));
            }
            return t;
        }

        // information_schema.tables
        if (lower == "information_schema.tables") {
            std::vector<Column> cols = {
                Column("table_catalog", "TEXT"),
                Column("table_schema",  "TEXT"),
                Column("table_name",    "TEXT"),
                Column("table_type",    "TEXT"),
            };
            Table t("information_schema.tables", cols);
            for (const auto& [tname, _tbl] : tables_) {
                t.insert(Row({"public", "public", bareName(tname), "BASE TABLE"}));
            }
            for (const auto& [vname, _vsql] : views_) {
                t.insert(Row({"public", "public", vname, "VIEW"}));
            }
            return t;
        }

        // information_schema.columns
        if (lower == "information_schema.columns") {
            std::vector<Column> cols = {
                Column("table_name",      "TEXT"),
                Column("column_name",     "TEXT"),
                Column("data_type",       "TEXT"),
                Column("ordinal_position","TEXT"),
            };
            Table t("information_schema.columns", cols);
            for (const auto& [tname, tbl] : tables_) {
                std::string bname = bareName(tname);
                const auto& tcols = tbl.columns();
                for (size_t i = 0; i < tcols.size(); ++i) {
                    t.insert(Row({
                        bname,
                        tcols[i].name,
                        tcols[i].type,
                        std::to_string(i + 1)
                    }));
                }
            }
            return t;
        }

        // Fallback: empty table
        return Table(name, {Column("result", "TEXT")});
    }
};

} // namespace milansql
