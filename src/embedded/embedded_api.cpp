// ============================================================
// embedded_api.cpp — Phase 117: Embedded Mode Implementation
//
// Implements the SQLite-compatible C API declared in
// include/milansql_embedded.h.  Uses the Engine + Parser
// directly (no dispatch.hpp) for clean result capture.
// ============================================================

#include "../../include/milansql_embedded.h"

// Pull in Engine + Parser (header-only)
#include "../engine/engine.hpp"
#include "../parser/parser.hpp"

#include <string>
#include <vector>
#include <memory>
#include <cstring>
#include <cstdlib>
#include <sstream>

// ── Internal result ───────────────────────────────────────────
struct EmbeddedResult {
    std::vector<std::string>              colNames;
    std::vector<std::vector<std::string>> rows;
    std::string                           errMsg;
    int                                   rowsAffected = 0;
    bool                                  ok  = true;
};

// ── EmbeddedExecutor: thin Engine+Parser wrapper ──────────────
class EmbeddedExecutor {
public:
    EmbeddedExecutor() = default;

    EmbeddedResult execute(const std::string& sql) {
        EmbeddedResult res;
        try {
            milansql::Parser parser;
            milansql::ParsedCommand cmd = parser.parse(sql);
            dispatch(cmd, sql, res);
        } catch (const std::exception& ex) {
            res.ok     = false;
            res.errMsg = ex.what();
        } catch (...) {
            res.ok     = false;
            res.errMsg = "Unknown error";
        }
        return res;
    }

    std::string lastError;
    int         lastChanges = 0;

private:
    milansql::Engine engine_;

    // ── Column names from Table ───────────────────────────────
    static std::vector<std::string> colNames(const milansql::Table& t) {
        std::vector<std::string> names;
        for (const auto& c : t.columns()) names.push_back(c.name);
        return names;
    }

    // ── Strip surrounding single-quotes from a stored value ──
    static std::string displayVal(const std::string& v) {
        if (v.size() >= 2 && v.front() == '\'' && v.back() == '\'')
            return v.substr(1, v.size() - 2);
        return v;
    }

    // ── Convert Table rows to string vectors ─────────────────
    static std::vector<std::vector<std::string>> tableRows(
            const milansql::Table& t) {
        std::vector<std::vector<std::string>> out;
        for (const auto& row : t.rows()) {
            if (row.xmax != 0) continue;   // skip MVCC-deleted rows
            std::vector<std::string> stripped;
            stripped.reserve(row.values.size());
            for (const auto& v : row.values)
                stripped.push_back(displayVal(v));
            out.push_back(std::move(stripped));
        }
        return out;
    }

    // ── Core dispatcher ───────────────────────────────────────
    void dispatch(const milansql::ParsedCommand& cmd,
                  const std::string& /*rawSql*/,
                  EmbeddedResult& res) {

        using CT = milansql::CommandType;

        switch (cmd.type) {

        // ── SELECT ──────────────────────────────────────────
        case CT::SELECT: {
            if (cmd.tableName.empty()) {
                res.ok = false; res.errMsg = "SELECT: missing table name"; break;
            }
            milansql::Table result;
            if (cmd.whereConds.empty()) {
                result = engine_.selectAll(cmd.tableName).clone();
            } else {
                auto qr = engine_.selectWhere(cmd.tableName,
                                              cmd.whereConds,
                                              cmd.whereLogic);
                result = std::move(qr.table);
            }
            res.colNames = colNames(result);
            res.rows     = tableRows(result);
            break;
        }

        // ── INSERT ──────────────────────────────────────────
        case CT::INSERT: {
            if (cmd.tableName.empty()) {
                res.ok = false; res.errMsg = "INSERT: missing table name"; break;
            }
            const std::vector<std::vector<std::string>>& allVals =
                cmd.multiValues.empty()
                    ? std::vector<std::vector<std::string>>{cmd.values}
                    : cmd.multiValues;
            for (const auto& vals : allVals)
                engine_.insertRow(cmd.tableName, vals);
            res.rowsAffected     = static_cast<int>(allVals.size());
            lastChanges          = res.rowsAffected;
            res.colNames         = {"rows_affected"};
            res.rows             = {{std::to_string(res.rowsAffected)}};
            break;
        }

        // ── DELETE ──────────────────────────────────────────
        case CT::DELETE: {
            if (cmd.tableName.empty()) {
                res.ok = false; res.errMsg = "DELETE: missing table name"; break;
            }
            std::size_t n = 0;
            if (cmd.whereColumn.empty())
                n = engine_.deleteAll(cmd.tableName);
            else
                n = engine_.deleteWhere(cmd.tableName,
                                        cmd.whereColumn,
                                        cmd.whereValue);
            res.rowsAffected = static_cast<int>(n);
            lastChanges      = res.rowsAffected;
            res.colNames     = {"rows_affected"};
            res.rows         = {{std::to_string(n)}};
            break;
        }

        // ── UPDATE ──────────────────────────────────────────
        case CT::UPDATE: {
            if (cmd.tableName.empty()) {
                res.ok = false; res.errMsg = "UPDATE: missing table name"; break;
            }
            std::size_t n = 0;
            if (!cmd.setColumn.empty()) {
                if (cmd.whereColumn.empty())
                    n = engine_.updateAll(cmd.tableName,
                                          cmd.setColumn, cmd.setValue);
                else
                    n = engine_.updateWhere(cmd.tableName,
                                            cmd.setColumn,  cmd.setValue,
                                            cmd.whereColumn, cmd.whereValue);
            }
            res.rowsAffected = static_cast<int>(n);
            lastChanges      = res.rowsAffected;
            res.colNames     = {"rows_affected"};
            res.rows         = {{std::to_string(n)}};
            break;
        }

        // ── CREATE TABLE ────────────────────────────────────
        case CT::CREATE_TABLE: {
            if (cmd.tableName.empty() || cmd.columns.empty()) {
                res.ok = false; res.errMsg = "CREATE TABLE: missing name or columns"; break;
            }
            engine_.createTable(cmd.tableName, cmd.columns, cmd.foreignKeys);
            // Auto-create PK index
            for (const auto& col : cmd.columns) {
                if (col.isPrimaryKey) {
                    try { engine_.createIndex(cmd.tableName, {col.name}, "PRIMARY"); }
                    catch (...) {}
                    break;
                }
            }
            res.colNames = {"result"};
            res.rows     = {{"OK"}};
            break;
        }

        // ── DROP TABLE ──────────────────────────────────────
        case CT::DROP_TABLE: {
            if (cmd.tableName.empty()) {
                res.ok = false; res.errMsg = "DROP TABLE: missing name"; break;
            }
            engine_.dropTable(cmd.tableName);
            res.colNames = {"result"};
            res.rows     = {{"OK"}};
            break;
        }

        // ── CREATE INDEX ────────────────────────────────────
        case CT::CREATE_INDEX: {
            engine_.createIndex(cmd.tableName, cmd.indexColumns,
                                cmd.indexName);
            res.colNames = {"result"};
            res.rows     = {{"OK"}};
            break;
        }

        // ── TRUNCATE ────────────────────────────────────────
        case CT::TRUNCATE: {
            engine_.truncateTable(cmd.tableName);
            res.colNames = {"result"};
            res.rows     = {{"OK"}};
            break;
        }

        // ── Transaction stubs (no-op for embedded) ──────────
        case CT::BEGIN:
        case CT::COMMIT:
        case CT::ROLLBACK:
            res.colNames = {"result"};
            res.rows     = {{"OK"}};
            break;

        // ── Unrecognised / unsupported ───────────────────────
        default:
            res.ok = false;
            res.errMsg = "Unsupported command in embedded mode";
            break;
        }
    }
};

// ════════════════════════════════════════════════════════════════
// C API structs
// ════════════════════════════════════════════════════════════════

struct milansql_db {
    EmbeddedExecutor executor;
};

struct milansql_stmt {
    EmbeddedResult              result;
    int                         currentRow = -1;   // -1 = not started
    milansql_db                *db         = nullptr;
};

// ── Helpers ───────────────────────────────────────────────────
static char* dupString(const std::string& s) {
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (p) std::memcpy(p, s.c_str(), s.size() + 1);
    return p;
}

// ════════════════════════════════════════════════════════════════
// Public C API
// ════════════════════════════════════════════════════════════════

extern "C" {

int milansql_open(const char* /*filename*/, milansql_db** ppDb) {
    if (!ppDb) return MILANSQL_ERROR;
    *ppDb = new milansql_db();
    return MILANSQL_OK;
}

int milansql_close(milansql_db* db) {
    delete db;
    return MILANSQL_OK;
}

int milansql_exec(milansql_db*           db,
                  const char*            sql,
                  milansql_exec_callback callback,
                  void*                  callbackData,
                  char**                 errmsg) {
    if (!db || !sql) return MILANSQL_ERROR;

    EmbeddedResult res = db->executor.execute(sql);
    if (!res.ok) {
        db->executor.lastError = res.errMsg;
        if (errmsg) *errmsg = dupString(res.errMsg);
        return MILANSQL_ERROR;
    }

    if (callback && !res.rows.empty()) {
        // Build C-string arrays for the callback
        int ncols = static_cast<int>(res.colNames.size());
        std::vector<char*> colVals(ncols);
        std::vector<char*> colNames(ncols);

        for (int i = 0; i < ncols; ++i)
            colNames[i] = const_cast<char*>(res.colNames[i].c_str());

        for (const auto& row : res.rows) {
            for (int i = 0; i < ncols; ++i) {
                colVals[i] = (i < static_cast<int>(row.size()))
                    ? const_cast<char*>(row[i].c_str()) : nullptr;
            }
            int rc = callback(callbackData, ncols,
                              colVals.data(), colNames.data());
            if (rc != 0) break;   // callback requested abort
        }
    }

    if (errmsg) *errmsg = nullptr;
    return MILANSQL_OK;
}

int milansql_prepare(milansql_db*   db,
                     const char*    sql,
                     milansql_stmt** ppStmt) {
    if (!db || !sql || !ppStmt) return MILANSQL_ERROR;

    auto* stmt     = new milansql_stmt();
    stmt->db       = db;
    stmt->result   = db->executor.execute(sql);
    stmt->currentRow = -1;

    if (!stmt->result.ok) {
        db->executor.lastError = stmt->result.errMsg;
        delete stmt;
        *ppStmt = nullptr;
        return MILANSQL_ERROR;
    }

    *ppStmt = stmt;
    return MILANSQL_OK;
}

int milansql_step(milansql_stmt* stmt) {
    if (!stmt) return MILANSQL_ERROR;
    ++stmt->currentRow;
    if (stmt->currentRow < static_cast<int>(stmt->result.rows.size()))
        return MILANSQL_ROW;
    return MILANSQL_DONE;
}

int milansql_column_count(milansql_stmt* stmt) {
    if (!stmt) return 0;
    return static_cast<int>(stmt->result.colNames.size());
}

const char* milansql_column_name(milansql_stmt* stmt, int col) {
    if (!stmt) return nullptr;
    if (col < 0 || col >= static_cast<int>(stmt->result.colNames.size()))
        return nullptr;
    return stmt->result.colNames[col].c_str();
}

const char* milansql_column_text(milansql_stmt* stmt, int col) {
    if (!stmt || stmt->currentRow < 0) return nullptr;
    if (stmt->currentRow >= static_cast<int>(stmt->result.rows.size()))
        return nullptr;
    const auto& row = stmt->result.rows[stmt->currentRow];
    if (col < 0 || col >= static_cast<int>(row.size())) return nullptr;
    return row[col].c_str();
}

int milansql_finalize(milansql_stmt* stmt) {
    delete stmt;
    return MILANSQL_OK;
}

const char* milansql_errmsg(milansql_db* db) {
    if (!db) return "no database";
    return db->executor.lastError.c_str();
}

int milansql_changes(milansql_db* db) {
    if (!db) return 0;
    return db->executor.lastChanges;
}

void milansql_free(void* ptr) {
    std::free(ptr);
}

const char* milansql_version(void) {
    return "MilanSQL 5.8.0 Embedded";
}

} // extern "C"
