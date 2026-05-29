#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>

#include "engine/engine.hpp"  // Column, Row, WhereCondition, SelectItem, HavingCondition

// ============================================================
// parser.hpp — SQL-Parser für MilanSQL (Phase 24)
// Neu: Views (CREATE VIEW / DROP VIEW)
// ============================================================

namespace milansql {

enum class CommandType {
    SELECT,
    INSERT,
    UPDATE,
    DELETE,
    CREATE_TABLE,
    CREATE_INDEX,
    DROP_TABLE,
    DROP_INDEX,
    ALTER_TABLE,
    DESCRIBE,
    BEGIN,
    COMMIT,
    ROLLBACK,
    SHOW_TABLES,
    SHOW_INDEXES,
    SHOW_CREATE_TABLE,
    TRUNCATE,
    CREATE_VIEW,
    DROP_VIEW,
    STATUS,
    HELP,
    EXIT,
    // Phase 43: Triggers
    CREATE_TRIGGER,
    DROP_TRIGGER,
    SHOW_TRIGGERS,
    // Phase 44: Stored Procedures
    CREATE_PROCEDURE,
    DROP_PROCEDURE,
    SHOW_PROCEDURES,
    CALL_PROCEDURE,
    // Phase 45: Prepared Statements
    PREPARE_STMT,
    EXECUTE_STMT,
    DEALLOCATE_STMT,
    SHOW_PREPARED,
    // Phase 46: User Management
    CREATE_USER,
    DROP_USER,
    SHOW_USERS,
    GRANT_PRIV,
    REVOKE_PRIV,
    SHOW_GRANTS,
    CONNECT_USER,
    DISCONNECT_USER,
    // Phase 49: Full-Text Search
    CREATE_FULLTEXT_INDEX,
    DROP_FULLTEXT_INDEX,
    // Phase 51: Schemas / Namespaces
    CREATE_SCHEMA,
    DROP_SCHEMA,
    SHOW_SCHEMAS,
    USE_SCHEMA,
    SHOW_TABLES_IN,
    // Phase 54A: Query Cache
    SHOW_CACHE,
    CLEAR_CACHE,
    SET_CACHE,
    // Phase 54B: EXPLAIN ANALYZE (flag on ParsedCommand, no separate type)
    // Phase 54D: SHOW PROCESSLIST
    SHOW_PROCESSLIST,
    // Phase 57: Backup / Restore
    BACKUP_DATABASE,
    BACKUP_TABLE,
    RESTORE_DATABASE,
    SHOW_BACKUPS,
    // Phase 58: Benchmark
    BENCHMARK,
    // Phase 59: Replication
    SHOW_MASTER_STATUS,
    SHOW_SLAVE_STATUS,
    SHOW_BINLOG,
    STOP_SLAVE,
    START_SLAVE,
    // Phase 60: CSV Import/Export
    LOAD_DATA,
    INTO_OUTFILE,
    SHOW_DATAFILES,
    // Phase 61: Event Scheduler
    CREATE_EVENT,
    DROP_EVENT,
    SHOW_EVENTS,
    ALTER_EVENT,
    SET_EVENT_SCHEDULER,
    // Phase 62: Partitioning
    SHOW_PARTITIONS,
    // Phase 64: SAVEPOINT
    SAVEPOINT,
    ROLLBACK_TO_SAVEPOINT,
    RELEASE_SAVEPOINT,
    // Phase 65: Locking
    LOCK_TABLE,
    UNLOCK_TABLES,
    SHOW_LOCKS,
    // Phase 69: Query Profiler
    PROFILE_ON,
    PROFILE_OFF,
    SHOW_PROFILES,
    SHOW_PROFILE_FOR_QUERY,
    // Phase 70: Spatial Index
    CREATE_SPATIAL_INDEX,
    // Phase 71: MVCC
    VACUUM,
    VACUUM_ANALYZE,
    SHOW_TRANSACTIONS,
    SET_TRANSACTION_ISOLATION,
    // Phase 72: WAL Recovery + Materialized Views
    SHOW_RECOVERY_STATUS,
    CREATE_MATERIALIZED_VIEW,
    REFRESH_MATERIALIZED_VIEW,
    DROP_MATERIALIZED_VIEW,
    SHOW_MATERIALIZED_VIEWS,
    // Phase 73: Buffer Pool Manager
    SHOW_BUFFER_POOL_STATUS,
    SET_BUFFER_POOL_SIZE,
    FLUSH_BUFFER_POOL,
    // Phase 75: Row-Level Security
    CREATE_POLICY,
    DROP_POLICY,
    SHOW_POLICIES_ON,
    // Phase 76: LISTEN / NOTIFY / UNLISTEN
    LISTEN,
    UNLISTEN,
    NOTIFY,
    SHOW_LISTEN,
    // Phase 77: Parallel Query
    SET_PARALLEL_THRESHOLD,
    SET_MAX_PARALLEL_WORKERS,
    SHOW_PARALLEL_STATUS,
    UNKNOWN
};

struct ParsedCommand {
    CommandType              type         = CommandType::UNKNOWN;
    std::string              raw;
    std::string              tableName;
    std::vector<Column>      columns;
    std::vector<std::string> values;

    // WHERE (Phase 9: mehrere Bedingungen mit AND/OR)
    std::vector<WhereCondition> whereConds;
    std::string                 whereLogic = "AND";  // "AND" oder "OR"

    // Backward-compat für UPDATE/DELETE (nur erste Bedingung)
    std::string              whereColumn;
    std::string              whereValue;
    std::string              whereOp = "=";

    // UPDATE SET (single, backward compat)
    std::string              setColumn;
    std::string              setValue;

    // Phase 22: Multi-Column UPDATE
    std::vector<std::string> updateCols;
    std::vector<std::string> updateVals;

    // Indizes
    std::string              indexName;
    std::vector<std::string> indexColumns;  // Phase 35: ein oder mehrere Spalten

    // SELECT-Optionen (Phase 8 / Phase 38)
    bool                     isCount       = false;
    int                      limit         = -1;
    int                      limitOffset   = 0;   // Phase 38: OFFSET
    std::vector<std::string> selectColumns;
    bool                     isDistinct    = false;
    std::vector<std::pair<std::string,bool>> orderByCols;  // Phase 38: {col, desc}

    // Aggregatfunktionen (Phase 9)
    bool                     isAggregate   = false;
    std::string              aggFunc;   // MIN, MAX, AVG, SUM
    std::string              aggCol;    // Spaltenname

    // GROUP BY (Phase 10)
    bool                          isGroupBy   = false;
    std::vector<SelectItem>       selectItems;   // SELECT-Liste für GROUP BY
    std::vector<std::string>      groupByCols;
    std::vector<HavingCondition>  havingConds;
    std::string                   havingLogic = "AND";

    // JOIN (Phase 12: mehrere JOINs, INNER + LEFT)
    bool                     isJoin      = false;
    std::vector<JoinClause>  joinClauses;

    // ALTER TABLE (Phase 16)
    std::string              alterOp;       // "ADD", "DROP", "RENAME"
    std::string              alterColName;  // Spaltenname (ADD/DROP/RENAME-alt)
    std::string              alterColType;  // Typ (nur ADD)
    std::string              alterColNew;   // Neuer Name (nur RENAME)

    // Subquery (Phase 14): WHERE col IN (SELECT col FROM tbl [WHERE ...])
    // Jede Subquery wird als vorberechnete inList in der jeweiligen
    // WhereCondition gespeichert — der Parser löst die Subquery nicht auf,
    // stattdessen merkt er sich die Rohdaten und main.cpp löst sie auf.
    struct SubquerySpec {
        size_t      condIdx;   // Index in whereConds, die diese Subquery gehört
        std::string subTable;
        std::string subCol;
        std::vector<WhereCondition> subWhere;
        std::string subWhereLogic = "AND";
    };
    std::vector<SubquerySpec>  subqueries;

    // Phase 20: FOREIGN KEY Definitionen aus CREATE TABLE
    std::vector<ForeignKeyDef> foreignKeys;

    // Phase 24: CREATE VIEW
    std::string viewSql;

    // Phase 27: Multi-row INSERT
    std::vector<std::vector<std::string>> multiValues;

    // Phase 55: Named-column INSERT (INSERT INTO t (col1, col3) VALUES ...)
    std::vector<std::string> insertColumnNames;

    // Phase 28: INSERT INTO ... SELECT ...
    bool        isInsertSelect  = false;
    std::string insertSelectSql;

    // Phase 39: UPSERT-Modus ("" = normal, "REPLACE", "IGNORE")
    std::string upsertMode;

    // Phase 30: Mengenoperationen (UNION / UNION ALL / INTERSECT / EXCEPT)
    bool        isSetOp  = false;
    std::string setOp;       // "UNION", "UNION ALL", "INTERSECT", "EXCEPT"
    std::string rightSql;    // rechte SELECT-Seite als Rohtext

    // Phase 31: CASE WHEN THEN ELSE END in SELECT-Liste
    bool hasCaseItems = false;

    // Phase 36: EXPLAIN-Modus (kein echtes Ausführen, nur Plan anzeigen)
    bool isExplain = false;
    // Phase 54B: EXPLAIN ANALYZE — echte Ausführung mit Zeitmessung
    bool isExplainAnalyze = false;
    // Phase 54A: SET CACHE ON/OFF
    std::string cacheEnabled;  // "ON" or "OFF"

    // Phase 37: Alias der Haupttabelle (z.B. "m" in FROM mitarbeiter m)
    std::string tableAlias;

    // Phase 41: WITH / CTE — Common Table Expressions
    // Jeder Eintrag: {cte_name, inner_sql}
    std::vector<std::pair<std::string,std::string>> cteList;

    // Phase 43: Trigger fields
    std::string triggerName;
    std::string triggerTiming;        // BEFORE / AFTER
    std::string triggerEvent;         // INSERT / UPDATE / DELETE
    std::string triggerTable;         // table name the trigger is on
    std::string triggerBody;          // raw body text (between BEGIN and END)
    std::string showTriggersTable;    // for "SHOW TRIGGERS ON tablename"

    // Phase 44: Stored Procedure fields
    std::string procedureName;
    std::vector<std::pair<std::string,std::string>> procedureParams; // {name, type}
    std::string procedureBody;
    std::vector<std::string> callArgs; // arguments for CALL

    // Phase 45: Prepared Statement fields
    std::string preparedName;          // for PREPARE, EXECUTE, DEALLOCATE
    std::string preparedSql;           // for PREPARE: the SQL after AS
    std::vector<std::string> execArgs; // for EXECUTE: the arguments

    // Phase 49: Full-Text Index fields
    std::string              fulltextIndexName;
    std::vector<std::string> fulltextCols;

    // Phase 46: User Management fields
    std::string userName;           // for user commands
    std::string userPassword;       // for CREATE USER / CONNECT
    std::string grantTable;         // for GRANT/REVOKE
    std::vector<std::string> grantPrivs; // for GRANT/REVOKE (list of privileges)
    std::string grantTargetUser;    // for GRANT TO / REVOKE FROM / SHOW GRANTS FOR

    // Phase 51: Schema fields
    std::string schemaName;         // for schema commands (CREATE/DROP/USE/SHOW TABLES IN)

    // Phase 57: Backup / Restore
    std::string backupFile;         // Dateipfad für BACKUP/RESTORE
    bool        ifExists = false;   // für DROP TABLE IF EXISTS

    // Phase 58: Benchmark
    int         benchmarkIter = 0;  // Anzahl Iterationen
    std::string benchmarkSql;       // SQL-Statement zum Benchmarken

    // Phase 60: CSV Import / Export
    std::string csvFile;                // Dateipfad für LOAD DATA / INTO OUTFILE
    std::string csvSeparator;           // Trennzeichen (default ",")
    bool        csvSkipHeader = false;  // erste Zeile überspringen

    // Phase 61: Event Scheduler
    std::string eventName;             // Event-Name
    std::string eventSql;              // DO <sql>
    bool        eventRecurring = true; // true=EVERY, false=AT (once)
    long long   eventIntervalN = 0;    // n in EVERY n UNIT
    std::string eventIntervalUnit;     // SECOND/MINUTE/HOUR/DAY/WEEK/MONTH
    bool        eventHasAt = false;    // EVERY n DAY AT 'HH:MM:SS'
    std::string eventAtTime;           // "HH:MM:SS" (hasAt) or "YYYY-MM-DD HH:MM:SS" (once)
    bool        eventEnabled = true;   // for ALTER EVENT ENABLE/DISABLE
    bool        eventSchedulerOn = true; // for SET EVENT_SCHEDULER = ON/OFF

    // Phase 62: Partitioning
    // Used by CREATE TABLE parser to pass partition info to dispatch
    struct ParsedPartitionRange {
        std::string name;
        std::string limitStr;  // "100" or "MAXVALUE"
    };
    struct ParsedPartitionList {
        std::string name;
        std::vector<std::string> values;
    };
    std::string partitionType;   // "RANGE", "LIST", "HASH", or ""
    std::string partitionColumn; // column name
    int         partitionHashCount = 0;
    std::vector<ParsedPartitionRange> partitionRanges;
    std::vector<ParsedPartitionList>  partitionLists;
    // For SHOW PARTITIONS / ALTER TABLE DROP PARTITION
    std::string partitionName;   // partition name for DROP PARTITION
    // For ALTER TABLE ADD PARTITION (RANGE)
    ParsedPartitionRange addRangeDef;
    // For ALTER TABLE ADD PARTITION (LIST)
    ParsedPartitionList  addListDef;
    // Phase 64: SAVEPOINT
    std::string savepointName;

    // Phase 65: SELECT FOR UPDATE / LOCK TABLE
    bool        isForUpdate = false;   // SELECT ... FOR UPDATE
    std::string lockType;              // "READ" or "WRITE" for LOCK TABLE

    // Phase 69: Query Profiler
    int         profileQueryId = 0;    // SHOW PROFILE FOR QUERY n

    // Phase 71: MVCC
    std::string isolationLevel;        // SET TRANSACTION ISOLATION LEVEL ...

    // Phase 72: Materialized Views
    std::string matViewName;           // CREATE/REFRESH/DROP MATERIALIZED VIEW name
    std::string matViewSql;            // CREATE MATERIALIZED VIEW ... AS <sql>

    // Phase 75: Row-Level Security
    std::string policyName;
    std::string policyCommand;
    std::string policyUser;
    std::string policyUsingExpr;

    // Phase 76: LISTEN / NOTIFY / UNLISTEN
    std::string channelName;
    std::string notifyPayload;

    // Phase 77: Parallel query hint
    int parallelHint = 0;  // from /*+ PARALLEL(N) */ comment
};

class Parser {
public:
    ParsedCommand parse(const std::string& inputRaw) {
        // Strip trailing semicolon and whitespace
        std::string input = inputRaw;
        while (!input.empty() && (input.back() == ';' || input.back() == ' ' || input.back() == '\t' || input.back() == '\r'))
            input.pop_back();

        ParsedCommand cmd;
        cmd.raw = input;

        // ── Phase 60: SELECT … INTO OUTFILE detection ────────────
        // Detect and strip trailing "INTO OUTFILE 'file' [SEPARATOR 'sep']"
        // before the SELECT is parsed, so the rest of parse() sees a clean SELECT.
        {
            std::string upInp;
            upInp.reserve(input.size());
            for (unsigned char c : input)
                upInp += static_cast<char>(std::toupper(c));
            const std::string needle = " INTO OUTFILE ";
            auto pos = upInp.rfind(needle);
            if (pos != std::string::npos) {
                std::string suffix = input.substr(pos + needle.size());
                // Extract quoted file path
                auto findQuoted = [](const std::string& s) -> std::string {
                    for (char q : {'\'', '"'}) {
                        auto q1 = s.find(q);
                        if (q1 != std::string::npos) {
                            auto q2 = s.find(q, q1 + 1);
                            if (q2 != std::string::npos)
                                return s.substr(q1 + 1, q2 - q1 - 1);
                        }
                    }
                    // unquoted: first token
                    size_t i = 0;
                    while (i < s.size() && s[i] == ' ') ++i;
                    size_t j = i;
                    while (j < s.size() && s[j] != ' ') ++j;
                    return s.substr(i, j - i);
                };
                std::string fp = findQuoted(suffix);
                if (!fp.empty()) {
                    cmd.csvFile      = fp;
                    cmd.csvSeparator = ",";
                    // Check for SEPARATOR keyword in suffix
                    std::string upSuffix;
                    for (unsigned char c : suffix) upSuffix += static_cast<char>(std::toupper(c));
                    auto sepKw = upSuffix.find("SEPARATOR");
                    if (sepKw != std::string::npos) {
                        std::string afterSep = suffix.substr(sepKw + 9);
                        cmd.csvSeparator = findQuoted(afterSep);
                        if (cmd.csvSeparator.empty()) cmd.csvSeparator = ",";
                    }
                    // Trim SELECT part
                    input    = input.substr(0, pos);
                    cmd.raw  = input;
                }
            }
        }

        // ── Phase 65: SELECT … FOR UPDATE — strip suffix ─────────
        {
            std::string upInpFU;
            upInpFU.reserve(input.size());
            for (unsigned char c : input) upInpFU += static_cast<char>(std::toupper(c));
            const std::string fuNeedle = " FOR UPDATE";
            if (upInpFU.size() >= fuNeedle.size() &&
                upInpFU.substr(upInpFU.size() - fuNeedle.size()) == fuNeedle) {
                cmd.isForUpdate = true;
                input   = input.substr(0, input.size() - fuNeedle.size());
                cmd.raw = input;
            }
        }

        // ── Phase 41: WITH / CTE erkennen ────────────────────────
        {
            auto ftFull = tokenizeFull(input);
            if (!ftFull.empty() && toUpper(ftFull[0]) == "WITH") {
                std::vector<std::pair<std::string,std::string>> ctes;
                size_t i = 1; // skip "WITH"
                while (i < ftFull.size()) {
                    // skip commas between CTEs
                    while (i < ftFull.size() && ftFull[i] == ",") ++i;
                    if (i >= ftFull.size()) break;
                    // check if this is the start of the main SELECT/UNION/etc.
                    std::string kw = toUpper(ftFull[i]);
                    if (kw == "SELECT" || kw == "INSERT" ||
                        kw == "UPDATE" || kw == "DELETE") break;
                    // expect: name AS ( ... )
                    std::string cteName = ftFull[i++];
                    if (i >= ftFull.size() || toUpper(ftFull[i]) != "AS") break;
                    ++i; // skip AS
                    if (i >= ftFull.size() || ftFull[i] != "(") break;
                    ++i; // skip opening (
                    // collect inner SQL tokens (depth-balanced)
                    int depth = 1;
                    std::vector<std::string> innerToks;
                    while (i < ftFull.size() && depth > 0) {
                        if (ftFull[i] == "(") ++depth;
                        else if (ftFull[i] == ")") {
                            --depth;
                            if (depth == 0) { ++i; break; }
                        }
                        if (depth > 0) innerToks.push_back(ftFull[i]);
                        ++i;
                    }
                    std::string innerSql;
                    for (size_t j = 0; j < innerToks.size(); ++j) {
                        if (j > 0) innerSql += " ";
                        innerSql += innerToks[j];
                    }
                    ctes.push_back({cteName, innerSql});
                }
                // remaining tokens form the main query
                std::string mainSql;
                for (size_t j = i; j < ftFull.size(); ++j) {
                    if (j > i) mainSql += " ";
                    mainSql += ftFull[j];
                }
                if (!ctes.empty() && !mainSql.empty()) {
                    ParsedCommand result = parse(mainSql);
                    result.cteList = ctes;
                    result.raw = input;
                    return result;
                }
            }
        }

        // ── Phase 36/54B: EXPLAIN [ANALYZE] Prefix erkennen ─────
        {
            std::string up;
            size_t s = 0;
            while (s < input.size() && (input[s] == ' ' || input[s] == '\t')) ++s;
            for (size_t i = s; i < input.size(); ++i)
                up += static_cast<char>(std::toupper(static_cast<unsigned char>(input[i])));
            if (up.size() >= 7 && up.substr(0, 7) == "EXPLAIN" &&
                (up.size() == 7 || up[7] == ' ' || up[7] == '\t')) {
                size_t rest = s + 7;
                while (rest < input.size() && (input[rest] == ' ' || input[rest] == '\t'))
                    ++rest;
                // Phase 54B: check for ANALYZE keyword
                bool isAnalyze = false;
                std::string upRest;
                for (size_t i = rest; i < input.size(); ++i)
                    upRest += static_cast<char>(std::toupper(static_cast<unsigned char>(input[i])));
                if (upRest.size() >= 7 && upRest.substr(0, 7) == "ANALYZE" &&
                    (upRest.size() == 7 || upRest[7] == ' ' || upRest[7] == '\t')) {
                    isAnalyze = true;
                    rest += 7;
                    while (rest < input.size() && (input[rest] == ' ' || input[rest] == '\t'))
                        ++rest;
                }
                ParsedCommand inner = parse(input.substr(rest));
                inner.isExplain = true;
                if (isAnalyze) inner.isExplainAnalyze = true;
                return inner;
            }
        }

        // ── Transaktions-Befehle ──────────────────────────────────
        {
            auto st = tokenize(input);
            if (!st.empty()) {
                std::string k = toUpper(st[0]);
                if (k == "BEGIN")    { cmd.type = CommandType::BEGIN;    return cmd; }
                if (k == "COMMIT")   { cmd.type = CommandType::COMMIT;   return cmd; }
                if (k == "ROLLBACK") {
                    // ROLLBACK TO SAVEPOINT name  or  ROLLBACK TO name
                    if (st.size() >= 3 && toUpper(st[1]) == "TO") {
                        size_t ni = 2;
                        if (st.size() >= 4 && toUpper(st[2]) == "SAVEPOINT") ni = 3;
                        if (ni < st.size()) {
                            cmd.type = CommandType::ROLLBACK_TO_SAVEPOINT;
                            cmd.savepointName = st[ni];
                            return cmd;
                        }
                    }
                    cmd.type = CommandType::ROLLBACK;
                    return cmd;
                }
                // Phase 64: SAVEPOINT name
                if (k == "SAVEPOINT" && st.size() >= 2) {
                    cmd.type = CommandType::SAVEPOINT;
                    cmd.savepointName = st[1];
                    return cmd;
                }
                // Phase 64: RELEASE SAVEPOINT name
                if (k == "RELEASE" && st.size() >= 3 &&
                    toUpper(st[1]) == "SAVEPOINT") {
                    cmd.type = CommandType::RELEASE_SAVEPOINT;
                    cmd.savepointName = st[2];
                    return cmd;
                }
                // Phase 65: LOCK TABLE name READ|WRITE
                if (k == "LOCK" && st.size() >= 4 &&
                    toUpper(st[1]) == "TABLE") {
                    cmd.type      = CommandType::LOCK_TABLE;
                    cmd.tableName = st[2];
                    cmd.lockType  = toUpper(st[3]);  // "READ" or "WRITE"
                    return cmd;
                }
                // Phase 65: UNLOCK TABLES
                if (k == "UNLOCK" && st.size() >= 2 &&
                    toUpper(st[1]) == "TABLES") {
                    cmd.type = CommandType::UNLOCK_TABLES;
                    return cmd;
                }
                // Phase 65: SHOW LOCKS
                if (k == "SHOW" && st.size() >= 2 &&
                    toUpper(st[1]) == "LOCKS") {
                    cmd.type = CommandType::SHOW_LOCKS;
                    return cmd;
                }
            }
        }

        // ── Phase 51: Schema-Befehle ─────────────────────────────
        {
            auto st = tokenize(input);
            if (!st.empty()) {
                std::string k0 = toUpper(st[0]);
                std::string k1 = st.size() > 1 ? toUpper(st[1]) : "";
                std::string k2 = st.size() > 2 ? toUpper(st[2]) : "";
                std::string k3 = st.size() > 3 ? toUpper(st[3]) : "";
                // CREATE SCHEMA name
                if (k0 == "CREATE" && k1 == "SCHEMA") {
                    cmd.type = CommandType::CREATE_SCHEMA;
                    if (st.size() >= 3) cmd.schemaName = st[2];
                    else cmd.type = CommandType::UNKNOWN;
                    return cmd;
                }
                // DROP SCHEMA name
                if (k0 == "DROP" && k1 == "SCHEMA") {
                    cmd.type = CommandType::DROP_SCHEMA;
                    if (st.size() >= 3) cmd.schemaName = st[2];
                    else cmd.type = CommandType::UNKNOWN;
                    return cmd;
                }
                // SHOW SCHEMAS
                if (k0 == "SHOW" && k1 == "SCHEMAS") {
                    cmd.type = CommandType::SHOW_SCHEMAS;
                    return cmd;
                }
                // USE schemaname
                if (k0 == "USE") {
                    cmd.type = CommandType::USE_SCHEMA;
                    if (st.size() >= 2) cmd.schemaName = st[1];
                    else cmd.type = CommandType::UNKNOWN;
                    return cmd;
                }
                // SHOW TABLES IN schemaname
                if (k0 == "SHOW" && k1 == "TABLES" && k2 == "IN") {
                    cmd.type = CommandType::SHOW_TABLES_IN;
                    if (st.size() >= 4) cmd.schemaName = st[3];
                    else cmd.type = CommandType::UNKNOWN;
                    return cmd;
                }
            }
        }

        // ── ALTER TABLE-Erkennung ─────────────────────────────────
        {
            auto st = tokenize(input);
            if (st.size() >= 2 &&
                toUpper(st[0]) == "ALTER" && toUpper(st[1]) == "TABLE") {
                parseAlterTableCmd(st, cmd, input);
                return cmd;
            }
        }

        // ── Phase 30: Mengenoperationen (UNION/INTERSECT/EXCEPT) ──
        {
            auto st = tokenize(input);
            for (size_t i = 0; i < st.size(); ++i) {
                std::string u = toUpper(st[i]);
                if (u == "UNION" || u == "INTERSECT" || u == "EXCEPT") {
                    parseSetOp(input, cmd);
                    return cmd;
                }
            }
        }

        // ── JOIN-Erkennung (vor GROUP BY) ────────────────────────
        {
            auto st = tokenize(input);
            for (const auto& tok : st) {
                if (toUpper(tok) == "JOIN") {
                    parseJoinQuery(input, cmd);
                    return cmd;
                }
            }
        }

        // ── IN / BETWEEN / EXISTS / MATCH-Erkennung ──────────────
        {
            auto st = tokenize(input);
            if (!st.empty() && toUpper(st[0]) == "SELECT") {
                for (const auto& tok : st) {
                    std::string u = toUpper(tok);
                    if (u == "IN" || u == "BETWEEN" || u == "EXISTS" || u == "MATCH") {
                        parseSelectFull(input, cmd);
                        return cmd;
                    }
                }
            }
        }

        // ── GROUP BY-Erkennung ────────────────────────────────────
        {
            auto st = tokenize(input);
            for (size_t i = 0; i + 1 < st.size(); ++i) {
                if (toUpper(st[i]) == "GROUP" && toUpper(st[i + 1]) == "BY") {
                    parseGroupByQuery(input, cmd);
                    return cmd;
                }
            }
        }

        // ── Phase 31: CASE WHEN-Erkennung ────────────────────────
        {
            auto st = tokenize(input);
            if (!st.empty() && toUpper(st[0]) == "SELECT") {
                for (const auto& tok : st) {
                    if (toUpper(tok) == "CASE") {
                        parseSelectFull(input, cmd);
                        return cmd;
                    }
                }
            }
        }

        // ── Phase 37: Scalar Subquery in SELECT oder WHERE ───────
        {
            auto st37 = tokenize(input);
            if (!st37.empty() && toUpper(st37[0]) == "SELECT") {
                for (const auto& tok : st37) {
                    std::string u;
                    for (char c : tok) u += static_cast<char>(
                        std::toupper(static_cast<unsigned char>(c)));
                    // Token beginnt mit '(' und enthält SELECT
                    if (u.size() >= 7 && u.substr(0, 7) == "(SELECT") {
                        parseSelectFull(input, cmd);
                        return cmd;
                    }
                }
            }
        }

        // ── Phase 32: String-Funktionen-Erkennung ────────────────
        {
            static const std::vector<std::string> SFUNCS =
                {"UPPER", "LOWER", "LENGTH", "CONCAT", "SUBSTR", "TRIM", "REPLACE",
                 "ABS", "ROUND", "MOD", "POWER", "SQRT", "CEIL", "FLOOR",
                 "IFNULL", "COALESCE", "CAST", "MATCH",
                 // Phase 55: DATE/TIME-Funktionen
                 "NOW", "CURDATE", "CURTIME", "DATE", "TIME",
                 "YEAR", "MONTH", "DAY", "HOUR", "MINUTE", "SECOND",
                 "DATEDIFF", "DATE_ADD", "DATE_SUB", "DATE_FORMAT",
                 "CURRENT_TIMESTAMP", "CURRENT_DATE", "CURRENT_TIME",
                 // Phase 56: JSON-Funktionen
                 "JSON_EXTRACT", "JSON_SET", "JSON_KEYS", "JSON_LENGTH",
                 "JSON_CONTAINS", "JSON_TYPE", "JSON_VALID",
                 // Phase 64: REGEXP-Funktionen
                 "REGEXP_REPLACE", "REGEXP_EXTRACT",
                 // Phase 70: Spatial-Funktionen
                 "ST_DISTANCE", "ST_X", "ST_Y", "ST_WITHIN", "ST_ASTEXT"};
            auto st = tokenize(input);
            if (!st.empty() && toUpper(st[0]) == "SELECT") {
                for (const auto& tok : st) {
                    std::string u = toUpper(tok);
                    for (const auto& f : SFUNCS) {
                        size_t fl = f.size();
                        if (u == f ||
                            (u.size() >= fl + 1 &&
                             u.substr(0, fl) == f && u[fl] == '(')) {
                            parseSelectFull(input, cmd);
                            return cmd;
                        }
                    }
                }
            }
        }

        // ── Phase 42: Window Functions-Erkennung ─────────────────
        {
            static const std::vector<std::string> WFUNCS =
                {"ROW_NUMBER", "RANK", "DENSE_RANK", "SUM", "AVG", "COUNT", "MIN", "MAX"};
            auto st = tokenize(input);
            if (!st.empty() && toUpper(st[0]) == "SELECT") {
                // Check for OVER keyword (indicates window function)
                for (const auto& tok : st) {
                    if (toUpper(tok) == "OVER") {
                        parseSelectFull(input, cmd);
                        return cmd;
                    }
                }
            }
            (void)WFUNCS;  // suppress unused warning
        }

        // ── Phase 62: Pre-process CREATE TABLE ... PARTITION BY ... ──
        {
            std::string uinput = toUpper(input);
            // Check if this is CREATE TABLE with PARTITION BY
            if (uinput.size() > 12 && uinput.substr(0, 12) == "CREATE TABLE") {
                // Find first '(' — start of column definitions
                auto colStart = input.find('(');
                if (colStart != std::string::npos) {
                    // Find balanced ')' for column list
                    int depth = 0;
                    size_t colEnd = std::string::npos;
                    for (size_t ci = colStart; ci < input.size(); ++ci) {
                        if (input[ci] == '(') ++depth;
                        else if (input[ci] == ')') {
                            --depth;
                            if (depth == 0) { colEnd = ci; break; }
                        }
                    }
                    if (colEnd != std::string::npos && colEnd + 1 < input.size()) {
                        std::string remainder = input.substr(colEnd + 1);
                        std::string urem = toUpper(remainder);
                        // Check for PARTITION BY in remainder
                        auto pbPos = urem.find("PARTITION BY");
                        if (pbPos != std::string::npos) {
                            // Extract partition clause
                            std::string partClause = remainder.substr(pbPos + 12); // after "PARTITION BY"
                            while (!partClause.empty() && partClause.front() == ' ') partClause = partClause.substr(1);
                            std::string upart = toUpper(partClause);
                            // Determine type
                            std::string ptype;
                            size_t afterType = 0;
                            if (upart.substr(0, 5) == "RANGE") { ptype = "RANGE"; afterType = 5; }
                            else if (upart.substr(0, 4) == "LIST") { ptype = "LIST"; afterType = 4; }
                            else if (upart.substr(0, 4) == "HASH") { ptype = "HASH"; afterType = 4; }
                            if (!ptype.empty()) {
                                cmd.partitionType = ptype;
                                // Extract (column)
                                auto colParen = partClause.find('(', afterType);
                                if (colParen != std::string::npos) {
                                    auto colParenEnd = partClause.find(')', colParen);
                                    if (colParenEnd != std::string::npos) {
                                        std::string colInner = trim(partClause.substr(colParen + 1, colParenEnd - colParen - 1));
                                        cmd.partitionColumn = colInner;
                                        // Now look for PARTITIONS n (HASH) or partition list
                                        std::string rest = partClause.substr(colParenEnd + 1);
                                        std::string urest = toUpper(rest);
                                        // HASH: look for PARTITIONS n
                                        if (ptype == "HASH") {
                                            auto pn = urest.find("PARTITIONS");
                                            if (pn != std::string::npos) {
                                                std::string nstr = trim(rest.substr(pn + 10));
                                                try { cmd.partitionHashCount = std::stoi(nstr); } catch (...) {}
                                            }
                                        } else {
                                            // RANGE or LIST: parse (PARTITION p VALUES ...)
                                            auto listStart = rest.find('(');
                                            if (listStart != std::string::npos) {
                                                // Find balanced end
                                                int d2 = 0;
                                                size_t listEnd2 = std::string::npos;
                                                for (size_t ci2 = listStart; ci2 < rest.size(); ++ci2) {
                                                    if (rest[ci2] == '(') ++d2;
                                                    else if (rest[ci2] == ')') {
                                                        --d2;
                                                        if (d2 == 0) { listEnd2 = ci2; break; }
                                                    }
                                                }
                                                if (listEnd2 != std::string::npos) {
                                                    std::string partDefs = rest.substr(listStart + 1, listEnd2 - listStart - 1);
                                                    // Split by top-level commas
                                                    std::vector<std::string> pdParts;
                                                    {
                                                        int dep = 0;
                                                        std::string cur;
                                                        for (char c : partDefs) {
                                                            if (c == '(') { ++dep; cur += c; }
                                                            else if (c == ')') { --dep; cur += c; }
                                                            else if (c == ',' && dep == 0) {
                                                                pdParts.push_back(trim(cur)); cur.clear();
                                                            } else { cur += c; }
                                                        }
                                                        if (!trim(cur).empty()) pdParts.push_back(trim(cur));
                                                    }
                                                    for (auto& pd : pdParts) {
                                                        auto tpd = tokenize(pd);
                                                        if (tpd.size() < 2) continue;
                                                        // PARTITION name VALUES LESS THAN (n) or VALUES IN (v1,v2)
                                                        if (toUpper(tpd[0]) != "PARTITION") continue;
                                                        std::string pname = tpd[1];
                                                        std::string upd = toUpper(pd);
                                                        if (ptype == "RANGE") {
                                                            ParsedCommand::ParsedPartitionRange rdef;
                                                            rdef.name = pname;
                                                            auto vltPos = upd.find("VALUES LESS THAN");
                                                            if (vltPos != std::string::npos) {
                                                                auto vp = pd.find('(', vltPos);
                                                                auto vpe = pd.find(')', vp);
                                                                if (vp != std::string::npos && vpe != std::string::npos)
                                                                    rdef.limitStr = trim(pd.substr(vp + 1, vpe - vp - 1));
                                                                else rdef.limitStr = "MAXVALUE";
                                                            } else if (upd.find("MAXVALUE") != std::string::npos) {
                                                                rdef.limitStr = "MAXVALUE";
                                                            }
                                                            cmd.partitionRanges.push_back(rdef);
                                                        } else if (ptype == "LIST") {
                                                            ParsedCommand::ParsedPartitionList ldef;
                                                            ldef.name = pname;
                                                            auto viPos = upd.find("VALUES IN");
                                                            if (viPos != std::string::npos) {
                                                                auto vp = pd.find('(', viPos);
                                                                auto vpe = pd.rfind(')');
                                                                if (vp != std::string::npos && vpe != std::string::npos) {
                                                                    std::string vals = pd.substr(vp + 1, vpe - vp - 1);
                                                                    for (auto& v : splitTrim(vals, ',')) {
                                                                        std::string sv = v;
                                                                        if (sv.size() >= 2 && sv.front() == '\'' && sv.back() == '\'')
                                                                            sv = sv.substr(1, sv.size() - 2);
                                                                        ldef.values.push_back(sv);
                                                                    }
                                                                }
                                                            }
                                                            cmd.partitionLists.push_back(ldef);
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                                // Truncate input to remove PARTITION BY clause
                                input = input.substr(0, colEnd + 1);
                            }
                        }
                    }
                }
            }
        }

        // ── Standard-Parsing (Phasen 1–9) ─────────────────────

        std::string parenContent;
        std::string beforeParen = input;
        std::string afterParen;

        auto parenStart = input.find('(');
        if (parenStart != std::string::npos) {
            auto parenEnd = input.rfind(')');
            if (parenEnd != std::string::npos && parenEnd > parenStart) {
                parenContent = input.substr(parenStart + 1, parenEnd - parenStart - 1);
                beforeParen  = input.substr(0, parenStart);
                if (parenEnd + 1 < input.size())
                    afterParen = input.substr(parenEnd + 1);
            }
        }

        auto tokens = tokenize(beforeParen);
        if (tokens.empty()) { cmd.type = CommandType::UNKNOWN; return cmd; }

        std::string kw0 = toUpper(tokens[0]);
        std::string kw1 = tokens.size() > 1 ? toUpper(tokens[1]) : "";
        std::string kw2 = tokens.size() > 2 ? toUpper(tokens[2]) : "";
        std::string kw3 = tokens.size() > 3 ? toUpper(tokens[3]) : "";

        // ── SELECT ──────────────────────────────────────────────
        if (kw0 == "SELECT") {
            cmd.type = CommandType::SELECT;
            // Phase 77: /*+ PARALLEL(N) */ hint
            {
                auto hp = input.find("/*+");
                if (hp != std::string::npos) {
                    auto he = input.find("*/", hp);
                    if (he != std::string::npos) {
                        std::string h = input.substr(hp + 3, he - hp - 3);
                        std::string hu = h;
                        for (auto& c : hu) c = static_cast<char>(std::toupper(
                            static_cast<unsigned char>(c)));
                        auto pp = hu.find("PARALLEL");
                        if (pp != std::string::npos) {
                            auto lp = h.find('(', pp);
                            auto rp = h.find(')', pp);
                            if (lp != std::string::npos && rp != std::string::npos)
                                try { cmd.parallelHint = std::stoi(h.substr(lp+1, rp-lp-1)); }
                                catch (...) {}
                        }
                    }
                }
            }
            size_t idx = 1;

            // DISTINCT?
            if (idx < tokens.size() && toUpper(tokens[idx]) == "DISTINCT") {
                cmd.isDistinct = true; ++idx;
            }

            // Aggregatfunktion: MIN, MAX, AVG, SUM?
            static const std::vector<std::string> AGGFUNCS =
                {"MIN", "MAX", "AVG", "SUM"};
            if (idx < tokens.size()) {
                std::string kw = toUpper(tokens[idx]);
                bool isAgg = false;
                for (const auto& f : AGGFUNCS) if (kw == f) { isAgg = true; break; }

                if (isAgg) {
                    cmd.isAggregate = true;
                    cmd.aggFunc     = kw;
                    cmd.aggCol      = trim(parenContent);
                    auto at = tokenize(afterParen);
                    if (at.size() >= 2 && toUpper(at[0]) == "FROM") {
                        cmd.tableName = at[1];
                        if (at.size() > 2) parseWhere(at, 2, cmd);
                    } else { cmd.type = CommandType::UNKNOWN; }
                    return cmd;
                }
            }

            // COUNT(*)?
            if (idx < tokens.size() && toUpper(tokens[idx]) == "COUNT") {
                cmd.isCount = true;
                auto at = tokenize(afterParen);
                if (at.size() >= 2 && toUpper(at[0]) == "FROM") {
                    cmd.tableName = at[1];
                    if (at.size() > 2) parseWhere(at, 2, cmd);
                } else { cmd.type = CommandType::UNKNOWN; }
                return cmd;
            }

            // Normales SELECT — finde FROM
            size_t fromIdx = tokens.size();
            for (size_t i = idx; i < tokens.size(); ++i)
                if (toUpper(tokens[i]) == "FROM") { fromIdx = i; break; }

            if (fromIdx == tokens.size() || fromIdx + 1 >= tokens.size()) {
                cmd.type = CommandType::UNKNOWN;
            } else {
                std::string colList;
                for (size_t i = idx; i < fromIdx; ++i) {
                    if (i > idx) colList += " ";
                    colList += tokens[i];
                }
                for (const auto& c : splitTrim(colList, ','))
                    if (c != "*" && !c.empty())
                        cmd.selectColumns.push_back(c);

                cmd.tableName = tokens[fromIdx + 1];
                size_t rest = fromIdx + 2;
                parseWhere(tokens, rest, cmd);
                parseOrderBy(tokens, rest, cmd);
                parseLimit(tokens, rest, cmd);
            }

        // ── Phase 39: INSERT OR REPLACE / INSERT OR IGNORE ──────
        // Syntax: INSERT OR REPLACE INTO tbl VALUES (...)
        //         INSERT OR IGNORE  INTO tbl VALUES (...)
        } else if (kw0 == "INSERT" && kw1 == "OR" &&
                   (kw2 == "REPLACE" || kw2 == "IGNORE") && kw3 == "INTO") {
            cmd.type       = CommandType::INSERT;
            cmd.upsertMode = kw2;  // "REPLACE" or "IGNORE"
            // Rebuild a synthetic "INSERT INTO ..." string for parseValueGroups
            // by removing "OR REPLACE/IGNORE " from the input
            {
                std::string synth = input;
                // Replace first "OR REPLACE" / "OR IGNORE" (case-insensitive) with ""
                std::string upper = synth;
                for (char& c : upper)
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                std::string needle = std::string("OR ") + kw2 + " ";
                auto pos = upper.find(needle);
                if (pos != std::string::npos)
                    synth.erase(pos, needle.size());
                // Parse tableName and values from the cleaned-up string
                auto rt = tokenize(synth);
                if (rt.size() >= 3) {
                    cmd.tableName   = rt[2];
                    cmd.multiValues = parseValueGroups(synth);
                    if (!cmd.multiValues.empty())
                        cmd.values = cmd.multiValues[0];
                } else {
                    cmd.type = CommandType::UNKNOWN;
                }
            }

        // ── INSERT INTO ─────────────────────────────────────────
        // Phase 27: Multi-row VALUES  — INSERT INTO t VALUES (...),(...),...
        // Phase 28: INSERT-SELECT     — INSERT INTO t SELECT ... FROM ...
        } else if (kw0 == "INSERT" && kw1 == "INTO") {
            cmd.type = CommandType::INSERT;
            // Phase 39: ON CONFLICT DO NOTHING suffix → treat as INSERT OR IGNORE
            {
                std::string ap = afterParen;
                for (char& c : ap)
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                auto ocp = ap.find("ON CONFLICT");
                if (ocp != std::string::npos) cmd.upsertMode = "IGNORE";
            }
            if (tokens.size() >= 4) {
                cmd.tableName = tokens[2];
                if (toUpper(tokens[3]) == "SELECT") {
                    // INSERT INTO name SELECT ...
                    cmd.isInsertSelect = true;
                    // SELECT-Teil aus Original-Input extrahieren
                    std::string inputUp = input;
                    for (char& c : inputUp)
                        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    auto selPos = inputUp.find("SELECT");
                    if (selPos != std::string::npos)
                        cmd.insertSelectSql = trim(input.substr(selPos));
                    else
                        cmd.type = CommandType::UNKNOWN;
                } else {
                    // INSERT INTO name [( col1, col2, ... )] VALUES (...),...
                    // Phase 55: Spalten-Liste parsen, falls vorhanden
                    {
                        std::string up = input;
                        for (char& c : up)
                            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                        // Finde erstes "(" das NICHT direkt nach "VALUES" kommt
                        auto vpos = up.find("VALUES");
                        auto ppos = up.find('(');
                        // Gibt es ein ( VOR VALUES?  → das ist die Spalten-Liste
                        if (ppos != std::string::npos &&
                            (vpos == std::string::npos || ppos < vpos)) {
                            // Finde zugehörige ")"
                            auto closePpos = up.find(')', ppos + 1);
                            if (closePpos != std::string::npos) {
                                std::string colStr = input.substr(ppos + 1, closePpos - ppos - 1);
                                for (const auto& colTok : splitTrim(colStr, ','))
                                    if (!colTok.empty()) cmd.insertColumnNames.push_back(colTok);
                            }
                        }
                    }
                    cmd.multiValues = parseValueGroups(input);
                    if (!cmd.multiValues.empty())
                        cmd.values = cmd.multiValues[0];  // Backward-Compat
                }
            } else if (tokens.size() == 3) {
                cmd.tableName   = tokens[2];
                // Phase 55: INSERT INTO name (col1, col2, ...) VALUES (...)
                // tokens is from beforeParen → beforeParen = "INSERT INTO name "
                // Column list is in parenContent, which starts with "col1, col2, ...) VALUES ..."
                // Use raw input to parse column list
                {
                    std::string up = input;
                    for (char& c : up)
                        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    auto vpos = up.find("VALUES");
                    auto ppos = up.find('(');
                    if (ppos != std::string::npos &&
                        (vpos == std::string::npos || ppos < vpos)) {
                        auto closePpos = up.find(')', ppos + 1);
                        if (closePpos != std::string::npos) {
                            std::string colStr = input.substr(ppos + 1, closePpos - ppos - 1);
                            for (const auto& colTok : splitTrim(colStr, ','))
                                if (!colTok.empty()) cmd.insertColumnNames.push_back(colTok);
                        }
                    }
                }
                cmd.multiValues = parseValueGroups(input);
                if (!cmd.multiValues.empty())
                    cmd.values = cmd.multiValues[0];
            } else { cmd.type = CommandType::UNKNOWN; }

        // ── Phase 43: CREATE TRIGGER ─────────────────────────────
        // Syntax: CREATE TRIGGER name BEFORE/AFTER INSERT/UPDATE/DELETE ON tbl
        //         FOR EACH ROW BEGIN body END
        } else if (kw0 == "CREATE" && kw1 == "TRIGGER") {
            cmd.type = CommandType::CREATE_TRIGGER;
            // Use tokenizeFull on the full original input to properly handle quoted strings
            auto ftTrig = tokenizeFull(input);
            // ftTrig[0]=CREATE, [1]=TRIGGER, [2]=name, [3]=BEFORE/AFTER,
            // [4]=INSERT/UPDATE/DELETE, [5]=ON (skip), [6]=tableName,
            // [7]=FOR, [8]=EACH, [9]=ROW, [10]=BEGIN, body..., END
            size_t ti = 2;
            if (ti < ftTrig.size()) cmd.triggerName    = ftTrig[ti++];
            if (ti < ftTrig.size()) cmd.triggerTiming  = toUpper(ftTrig[ti++]);
            if (ti < ftTrig.size()) cmd.triggerEvent   = toUpper(ftTrig[ti++]);
            // skip ON
            if (ti < ftTrig.size() && toUpper(ftTrig[ti]) == "ON") ++ti;
            if (ti < ftTrig.size()) cmd.triggerTable   = ftTrig[ti++];
            // skip FOR EACH ROW
            while (ti < ftTrig.size() && toUpper(ftTrig[ti]) != "BEGIN") ++ti;
            if (ti < ftTrig.size()) ++ti;  // skip BEGIN
            // collect body until final END (not END IF)
            std::string body;
            // We need to find the final standalone END
            // Collect tokens until we hit END that is NOT followed by IF
            while (ti < ftTrig.size()) {
                std::string utok = toUpper(ftTrig[ti]);
                // Check for standalone END (not END IF)
                if (utok == "END") {
                    // Check next token — if it's IF, keep going
                    size_t nxt = ti + 1;
                    if (nxt < ftTrig.size() && toUpper(ftTrig[nxt]) == "IF") {
                        // This is END IF — include it in body
                        if (!body.empty()) body += " ";
                        body += ftTrig[ti++];
                        body += " ";
                        body += ftTrig[ti++];
                    } else {
                        // Standalone END — stop
                        break;
                    }
                } else {
                    if (!body.empty()) body += " ";
                    body += ftTrig[ti++];
                }
            }
            cmd.triggerBody = body;

        // ── Phase 44: CREATE PROCEDURE ───────────────────────────
        // Syntax: CREATE PROCEDURE name(p1 TYPE1, ...) BEGIN body END
        } else if (kw0 == "CREATE" && kw1 == "PROCEDURE") {
            cmd.type = CommandType::CREATE_PROCEDURE;
            {
                auto toks = tokenizeFull(input);
                size_t i = 2; // skip CREATE PROCEDURE
                // procedure name might have '(' attached (e.g. "erhoehe_gehalt(")
                if (i < toks.size()) {
                    std::string nameTok = toks[i++];
                    if (!nameTok.empty() && nameTok.back() == '(') {
                        cmd.procedureName = nameTok.substr(0, nameTok.size() - 1);
                        // treat as if '(' was next token
                        // parse params until ')'
                        while (i < toks.size() && toks[i] != ")") {
                            if (toks[i] == ",") { ++i; continue; }
                            std::string pname = toks[i++];
                            std::string ptype = (i < toks.size() &&
                                toks[i] != "," && toks[i] != ")") ? toks[i++] : "TEXT";
                            cmd.procedureParams.push_back({pname, ptype});
                        }
                        if (i < toks.size() && toks[i] == ")") ++i; // skip )
                    } else {
                        cmd.procedureName = nameTok;
                        // next token should be '('
                        if (i < toks.size() && toks[i] == "(") {
                            ++i; // skip (
                            while (i < toks.size() && toks[i] != ")") {
                                if (toks[i] == ",") { ++i; continue; }
                                std::string pname = toks[i++];
                                std::string ptype = (i < toks.size() &&
                                    toks[i] != "," && toks[i] != ")") ? toks[i++] : "TEXT";
                                cmd.procedureParams.push_back({pname, ptype});
                            }
                            if (i < toks.size()) ++i; // skip )
                        }
                    }
                }
                // skip to BEGIN
                while (i < toks.size() && toUpper(toks[i]) != "BEGIN") ++i;
                if (i < toks.size()) ++i; // skip BEGIN
                // collect body until standalone END
                std::string body;
                while (i < toks.size() && toUpper(toks[i]) != "END") {
                    if (!body.empty()) body += " ";
                    body += toks[i++];
                }
                cmd.procedureBody = body;
            }

        // ── Phase 44: CALL ────────────────────────────────────────
        // Syntax: CALL name(arg1, arg2, ...)
        } else if (kw0 == "CALL") {
            cmd.type = CommandType::CALL_PROCEDURE;
            {
                // Find procedure name and args from raw input
                std::string rest = input;
                // Skip leading whitespace and "CALL"
                size_t s = 0;
                while (s < rest.size() && (rest[s]==' '||rest[s]=='\t')) ++s;
                // skip CALL keyword
                s += 4; // length of "CALL"
                while (s < rest.size() && (rest[s]==' '||rest[s]=='\t')) ++s;
                rest = rest.substr(s);
                // Find '('
                size_t parenP = rest.find('(');
                if (parenP == std::string::npos) {
                    cmd.procedureName = rest;
                    // trim
                    while (!cmd.procedureName.empty() && cmd.procedureName.back() == ' ')
                        cmd.procedureName.pop_back();
                } else {
                    cmd.procedureName = rest.substr(0, parenP);
                    while (!cmd.procedureName.empty() && cmd.procedureName.back() == ' ')
                        cmd.procedureName.pop_back();
                    // parse args between ( and )
                    size_t closeP = rest.rfind(')');
                    std::string argsStr = rest.substr(parenP + 1,
                        closeP != std::string::npos ?
                            closeP - parenP - 1 : std::string::npos);
                    // split by commas
                    std::string cur;
                    for (char c : argsStr) {
                        if (c == ',') {
                            while (!cur.empty() && cur.front() == ' ') cur = cur.substr(1);
                            while (!cur.empty() && cur.back()  == ' ') cur.pop_back();
                            if (!cur.empty()) cmd.callArgs.push_back(cur);
                            cur = "";
                        } else cur += c;
                    }
                    while (!cur.empty() && cur.front() == ' ') cur = cur.substr(1);
                    while (!cur.empty() && cur.back()  == ' ') cur.pop_back();
                    if (!cur.empty()) cmd.callArgs.push_back(cur);
                }
            }

        // ── Phase 44: DROP PROCEDURE ──────────────────────────────
        } else if (kw0 == "DROP" && kw1 == "PROCEDURE") {
            cmd.type = CommandType::DROP_PROCEDURE;
            if (tokens.size() >= 3) cmd.procedureName = tokens[2];
            else cmd.type = CommandType::UNKNOWN;

        // ── Phase 49: CREATE FULLTEXT INDEX ──────────────────────
        // Syntax: CREATE FULLTEXT INDEX name ON table (col1, col2, ...)
        } else if (kw0 == "CREATE" && kw1 == "FULLTEXT" && kw2 == "INDEX") {
            cmd.type = CommandType::CREATE_FULLTEXT_INDEX;
            {
                auto toks = tokenizeFull(input);
                // toks: CREATE FULLTEXT INDEX name ON table ( col1 , col2 )
                size_t i = 3;
                if (i < toks.size()) cmd.fulltextIndexName = toks[i++];
                if (i < toks.size() && toUpper(toks[i]) == "ON") ++i;
                if (i < toks.size()) cmd.tableName = toks[i++];
                if (i < toks.size() && toks[i] == "(") ++i;
                while (i < toks.size() && toks[i] != ")") {
                    if (toks[i] != ",") cmd.fulltextCols.push_back(toks[i]);
                    ++i;
                }
            }

        // ── Phase 49: DROP FULLTEXT INDEX ────────────────────────
        // Syntax: DROP FULLTEXT INDEX name ON table
        } else if (kw0 == "DROP" && kw1 == "FULLTEXT" && kw2 == "INDEX") {
            cmd.type = CommandType::DROP_FULLTEXT_INDEX;
            {
                auto toks = tokenizeFull(input);
                size_t i = 3;
                if (i < toks.size()) cmd.fulltextIndexName = toks[i++];
                if (i < toks.size() && toUpper(toks[i]) == "ON") ++i;
                if (i < toks.size()) cmd.tableName = toks[i];
            }

        // ── Phase 70: CREATE SPATIAL INDEX ───────────────────────
        // Syntax: CREATE SPATIAL INDEX name ON table (col)
        } else if (kw0 == "CREATE" && kw1 == "SPATIAL" && kw2 == "INDEX") {
            cmd.type = CommandType::CREATE_SPATIAL_INDEX;
            {
                auto toks = tokenizeFull(input);
                // CREATE SPATIAL INDEX name ON table ( col )
                size_t i = 3;
                if (i < toks.size()) cmd.indexName = toks[i++];
                if (i < toks.size() && toUpper(toks[i]) == "ON") ++i;
                if (i < toks.size()) cmd.tableName = toks[i++];
                if (i < toks.size() && toks[i] == "(") ++i;
                while (i < toks.size() && toks[i] != ")") {
                    if (toks[i] != ",") cmd.indexColumns.push_back(toks[i]);
                    ++i;
                }
            }

        // ── CREATE INDEX ─────────────────────────────────────────
        } else if (kw0 == "CREATE" && kw1 == "INDEX") {
            cmd.type = CommandType::CREATE_INDEX;
            if (tokens.size() >= 5 && toUpper(tokens[3]) == "ON") {
                cmd.indexName = tokens[2];
                cmd.tableName = tokens[4];
                // Phase 35: parenContent kann "col1, col2, ..." sein
                std::string content = trim(parenContent);
                std::stringstream ss(content);
                std::string part;
                while (std::getline(ss, part, ',')) {
                    std::string col = trim(part);
                    if (!col.empty()) cmd.indexColumns.push_back(col);
                }
            } else { cmd.type = CommandType::UNKNOWN; }

        // ── CREATE TABLE ─────────────────────────────────────────
        } else if (kw0 == "CREATE" && kw1 == "TABLE") {
            cmd.type = CommandType::CREATE_TABLE;
            if (tokens.size() >= 3) {
                cmd.tableName = tokens[2];
                for (const auto& colDef : splitTrim(parenContent, ',')) {
                    auto parts = tokenize(colDef);
                    if (parts.size() < 2) continue;

                    // FOREIGN KEY (fromCol) REFERENCES refTable(refCol) [ON DELETE action]
                    if (toUpper(parts[0]) == "FOREIGN" && parts.size() >= 5 &&
                        toUpper(parts[1]) == "KEY" &&
                        toUpper(parts[3]) == "REFERENCES") {
                        ForeignKeyDef fk;
                        fk.fromCol = extractParen(parts[2]);
                        auto dotSplit = splitRefPart(parts[4]);
                        fk.refTable = dotSplit.first;
                        fk.refCol   = dotSplit.second;
                        // Phase 21: ON DELETE CASCADE / SET NULL / RESTRICT
                        for (size_t oi = 5; oi + 2 < parts.size(); ++oi) {
                            if (toUpper(parts[oi]) == "ON" &&
                                toUpper(parts[oi + 1]) == "DELETE") {
                                std::string action = toUpper(parts[oi + 2]);
                                if (action == "CASCADE")
                                    fk.onDelete = "CASCADE";
                                else if (action == "RESTRICT")
                                    fk.onDelete = "RESTRICT";
                                else if (action == "SET" &&
                                         oi + 3 < parts.size() &&
                                         toUpper(parts[oi + 3]) == "NULL")
                                    fk.onDelete = "SET NULL";
                                break;
                            }
                        }
                        if (!fk.fromCol.empty() && !fk.refTable.empty() &&
                            !fk.refCol.empty())
                            cmd.foreignKeys.push_back(std::move(fk));
                        continue;
                    }

                    Column col(parts[0], toUpper(parts[1]));
                    for (size_t i = 2; i < parts.size(); ++i) {
                        std::string u = toUpper(parts[i]);
                        if (u == "NOT" && i + 1 < parts.size() &&
                            toUpper(parts[i + 1]) == "NULL") {
                            col.notNull = true; ++i;
                        } else if (u == "UNIQUE") {
                            col.isUnique = true;
                        } else if (u == "DEFAULT" && i + 1 < parts.size()) {
                            col.hasDefault   = true;
                            col.defaultValue = parts[++i];
                        } else if (u == "PRIMARY" && i + 1 < parts.size() &&
                                   toUpper(parts[i + 1]) == "KEY") {
                            col.isPrimaryKey = true;
                            col.notNull      = true;   // PRIMARY KEY impliziert NOT NULL
                            col.isUnique     = true;   // PRIMARY KEY impliziert UNIQUE
                            ++i;
                        } else if (u == "AUTO_INCREMENT") {
                            col.autoIncrement = true;
                        // Phase 68: GENERATED ALWAYS AS (expr) STORED|VIRTUAL
                        } else if (u == "GENERATED" && i + 2 < parts.size() &&
                                   toUpper(parts[i+1]) == "ALWAYS" && toUpper(parts[i+2]) == "AS") {
                            col.isGenerated = true;
                            i += 3; // skip ALWAYS AS
                            // collect tokens inside balanced parentheses
                            std::string exprTokens;
                            int depth = 0;
                            while (i < parts.size()) {
                                std::string pu = toUpper(parts[i]);
                                if (parts[i].front() == '(') { depth++; }
                                if (parts[i].back()  == ')') {
                                    // may be the closing paren
                                    if (!exprTokens.empty()) exprTokens += " ";
                                    exprTokens += parts[i];
                                    ++i;
                                    depth--;
                                    if (depth <= 0) break;
                                    continue;
                                }
                                if (!exprTokens.empty()) exprTokens += " ";
                                exprTokens += parts[i];
                                ++i;
                            }
                            // strip outer parens
                            while (!exprTokens.empty() && exprTokens.front() == '(') exprTokens = exprTokens.substr(1);
                            while (!exprTokens.empty() && exprTokens.back()  == ')') exprTokens.pop_back();
                            // trim
                            size_t es = exprTokens.find_first_not_of(" ");
                            size_t ee = exprTokens.find_last_not_of(" ");
                            col.generatedExpr = (es == std::string::npos) ? "" : exprTokens.substr(es, ee - es + 1);
                            // check for STORED or VIRTUAL keyword
                            if (i < parts.size()) {
                                std::string kw = toUpper(parts[i]);
                                if (kw == "STORED")  { col.isStored = true;  ++i; }
                                else if (kw == "VIRTUAL") { col.isStored = false; ++i; }
                            }
                            --i; // loop will ++i
                        // Phase 23: CHECK (colname op val)
                        } else if (u == "CHECK" && i + 3 < parts.size()) {
                            // parts[i+1] = "(colname"  (führende Klammer anhaftend)
                            // parts[i+2] = op
                            // parts[i+3] = "val)"      (abschließende Klammer(n) anhaftend)
                            std::string colPart = parts[i + 1];
                            if (!colPart.empty() && colPart.front() == '(')
                                colPart = colPart.substr(1);
                            std::string opStr = parts[i + 2];
                            std::string valPart = parts[i + 3];
                            while (!valPart.empty() && valPart.back() == ')')
                                valPart.pop_back();
                            if (toUpper(colPart) == toUpper(col.name) &&
                                !opStr.empty() && !valPart.empty()) {
                                CheckConstraint cc;
                                cc.op  = opStr;
                                cc.val = valPart;
                                col.checks.push_back(cc);
                            }
                            i += 3;
                        }
                    }
                    cmd.columns.push_back(std::move(col));
                }
            } else { cmd.type = CommandType::UNKNOWN; }

        // ── UPDATE ──────────────────────────────────────────────
        } else if (kw0 == "UPDATE") {
            cmd.type = CommandType::UPDATE;
            if (tokens.size() >= 4 && toUpper(tokens[2]) == "SET") {
                cmd.tableName = tokens[1];
                // WHERE-Position ermitteln
                size_t wherePos = tokens.size();
                for (size_t i = 3; i < tokens.size(); ++i)
                    if (toUpper(tokens[i]) == "WHERE") { wherePos = i; break; }
                // Phase 22: Multi-Column SET parsen
                parseMultiAssignment(tokens, 3, wherePos, cmd);
                parseWhere(tokens, wherePos, cmd);
            } else { cmd.type = CommandType::UNKNOWN; }

        // ── DELETE FROM ──────────────────────────────────────────
        } else if (kw0 == "DELETE" && kw1 == "FROM") {
            cmd.type = CommandType::DELETE;
            if (tokens.size() >= 3) {
                cmd.tableName = tokens[2];
                parseWhere(tokens, 3, cmd);
            } else { cmd.type = CommandType::UNKNOWN; }

        // ── Phase 43: DROP TRIGGER ───────────────────────────────
        } else if (kw0 == "DROP" && kw1 == "TRIGGER") {
            cmd.type = CommandType::DROP_TRIGGER;
            if (tokens.size() >= 3) cmd.triggerName = tokens[2];
            else cmd.type = CommandType::UNKNOWN;

        // ── DROP TABLE [IF EXISTS] ───────────────────────────────
        } else if (kw0 == "DROP" && kw1 == "TABLE") {
            cmd.type = CommandType::DROP_TABLE;
            size_t tblIdx = 2;
            if (tokens.size() >= 5 &&
                toUpper(tokens[2]) == "IF" && toUpper(tokens[3]) == "EXISTS") {
                cmd.ifExists = true;
                tblIdx = 4;
            }
            if (tokens.size() > tblIdx) cmd.tableName = tokens[tblIdx];
            else cmd.type = CommandType::UNKNOWN;

        // ── DROP INDEX ───────────────────────────────────────────
        } else if (kw0 == "DROP" && kw1 == "INDEX") {
            cmd.type = CommandType::DROP_INDEX;
            if (tokens.size() >= 5 && toUpper(tokens[3]) == "ON") {
                cmd.indexName = tokens[2];
                cmd.tableName = tokens[4];
            } else { cmd.type = CommandType::UNKNOWN; }

        // ── Phase 69: PROFILE ON / PROFILE OFF ───────────────────
        } else if (kw0 == "PROFILE") {
            if (kw1 == "ON")  cmd.type = CommandType::PROFILE_ON;
            else if (kw1 == "OFF") cmd.type = CommandType::PROFILE_OFF;
            else cmd.type = CommandType::UNKNOWN;

        // ── SHOW ─────────────────────────────────────────────────
        } else if (kw0 == "SHOW") {
            if (kw1 == "INDEXES" || kw1 == "INDICES") {
                cmd.type = CommandType::SHOW_INDEXES;
                if (tokens.size() >= 4 && toUpper(tokens[2]) == "FROM")
                    cmd.tableName = tokens[3];
                else cmd.type = CommandType::UNKNOWN;
            } else if (kw1 == "CREATE" && tokens.size() >= 4
                       && toUpper(tokens[2]) == "TABLE") {
                // SHOW CREATE TABLE name
                cmd.type      = CommandType::SHOW_CREATE_TABLE;
                cmd.tableName = tokens[3];
            // Phase 43: SHOW TRIGGERS [ON tablename]
            } else if (kw1 == "TRIGGERS") {
                cmd.type = CommandType::SHOW_TRIGGERS;
                if (tokens.size() >= 4 && toUpper(tokens[2]) == "ON")
                    cmd.showTriggersTable = tokens[3];
            // Phase 44: SHOW PROCEDURES
            } else if (kw1 == "PROCEDURES") {
                cmd.type = CommandType::SHOW_PROCEDURES;
            // Phase 45: SHOW PREPARED
            } else if (kw1 == "PREPARED") {
                cmd.type = CommandType::SHOW_PREPARED;
            // Phase 46: SHOW USERS
            } else if (kw1 == "USERS") {
                cmd.type = CommandType::SHOW_USERS;
            // Phase 46: SHOW GRANTS FOR user
            } else if (kw1 == "GRANTS") {
                cmd.type = CommandType::SHOW_GRANTS;
                if (tokens.size() >= 4 && toUpper(tokens[2]) == "FOR")
                    cmd.grantTargetUser = tokens[3];
            // Phase 54A: SHOW CACHE
            } else if (kw1 == "CACHE") {
                cmd.type = CommandType::SHOW_CACHE;
            // Phase 54D: SHOW PROCESSLIST
            } else if (kw1 == "PROCESSLIST") {
                cmd.type = CommandType::SHOW_PROCESSLIST;
            // Phase 57: SHOW BACKUPS
            } else if (kw1 == "BACKUPS") {
                cmd.type = CommandType::SHOW_BACKUPS;
            // Phase 58: SHOW STATUS (Alias für STATUS)
            } else if (kw1 == "STATUS") {
                cmd.type = CommandType::STATUS;
            // Phase 60: SHOW DATAFILES
            } else if (kw1 == "DATAFILES") {
                cmd.type = CommandType::SHOW_DATAFILES;
            // Phase 61: SHOW EVENTS
            } else if (kw1 == "EVENTS") {
                cmd.type = CommandType::SHOW_EVENTS;
            // Phase 62: SHOW PARTITIONS FROM table
            } else if (kw1 == "PARTITIONS" && tokens.size() >= 4 && toUpper(tokens[2]) == "FROM") {
                cmd.type = CommandType::SHOW_PARTITIONS;
                cmd.tableName = tokens[3];
            // Phase 69: SHOW PROFILES / SHOW PROFILE FOR QUERY n
            } else if (kw1 == "PROFILES") {
                cmd.type = CommandType::SHOW_PROFILES;
            } else if (kw1 == "PROFILE") {
                // SHOW PROFILE FOR QUERY n
                cmd.type = CommandType::SHOW_PROFILE_FOR_QUERY;
                // tokens: SHOW PROFILE FOR QUERY n
                if (tokens.size() >= 5 &&
                    toUpper(tokens[2]) == "FOR" && toUpper(tokens[3]) == "QUERY") {
                    try { cmd.profileQueryId = std::stoi(tokens[4]); } catch (...) {}
                }
            // Phase 71: SHOW TRANSACTIONS
            } else if (kw1 == "TRANSACTIONS") {
                cmd.type = CommandType::SHOW_TRANSACTIONS;
            // Phase 72: SHOW RECOVERY STATUS
            } else if (kw1 == "RECOVERY" && tokens.size() >= 3 && toUpper(tokens[2]) == "STATUS") {
                cmd.type = CommandType::SHOW_RECOVERY_STATUS;
            // Phase 72: SHOW MATERIALIZED VIEWS
            } else if (kw1 == "MATERIALIZED" && tokens.size() >= 3 && toUpper(tokens[2]) == "VIEWS") {
                cmd.type = CommandType::SHOW_MATERIALIZED_VIEWS;
            // Phase 73: SHOW BUFFER POOL STATUS
            } else if (kw1 == "BUFFER" && tokens.size() >= 4 &&
                       toUpper(tokens[2]) == "POOL" && toUpper(tokens[3]) == "STATUS") {
                cmd.type = CommandType::SHOW_BUFFER_POOL_STATUS;
            // Phase 75: SHOW POLICIES ON table
            } else if (kw1 == "POLICIES") {
                cmd.type = CommandType::SHOW_POLICIES_ON;
                for (size_t i = 2; i < tokens.size(); ++i)
                    if (toUpper(tokens[i]) == "ON" && i + 1 < tokens.size())
                        cmd.tableName = tokens[i + 1];
            // Phase 76: SHOW LISTEN
            } else if (kw1 == "LISTEN") {
                cmd.type = CommandType::SHOW_LISTEN;
            // Phase 77: SHOW PARALLEL STATUS
            } else if (kw1 == "PARALLEL" && tokens.size() >= 3 &&
                       toUpper(tokens[2]) == "STATUS") {
                cmd.type = CommandType::SHOW_PARALLEL_STATUS;
            // Phase 59: Replication SHOW commands
            } else if (kw1 == "MASTER" && kw2 == "STATUS") {
                cmd.type = CommandType::SHOW_MASTER_STATUS;
            } else if (kw1 == "SLAVE" && kw2 == "STATUS") {
                cmd.type = CommandType::SHOW_SLAVE_STATUS;
            } else if (kw1 == "BINLOG") {
                cmd.type = CommandType::SHOW_BINLOG;
            } else {
                cmd.type = CommandType::SHOW_TABLES;
            }

        // ── Phase 71: VACUUM / VACUUM ANALYZE ────────────────────
        } else if (kw0 == "VACUUM") {
            if (tokens.size() >= 2 && toUpper(tokens[1]) == "ANALYZE")
                cmd.type = CommandType::VACUUM_ANALYZE;
            else
                cmd.type = CommandType::VACUUM;

        // ── Phase 72: SHOW RECOVERY STATUS ───────────────────────
        } else if (kw0 == "SHOW" && kw1 == "RECOVERY" && tokens.size() >= 3 &&
                   toUpper(tokens[2]) == "STATUS") {
            cmd.type = CommandType::SHOW_RECOVERY_STATUS;

        // ── Phase 72: SHOW MATERIALIZED VIEWS ────────────────────
        } else if (kw0 == "SHOW" && kw1 == "MATERIALIZED" && tokens.size() >= 3 &&
                   toUpper(tokens[2]) == "VIEWS") {
            cmd.type = CommandType::SHOW_MATERIALIZED_VIEWS;

        // ── Phase 72: CREATE MATERIALIZED VIEW name AS <sql> ─────
        } else if (kw0 == "CREATE" && kw1 == "MATERIALIZED" && tokens.size() >= 5 &&
                   toUpper(tokens[2]) == "VIEW") {
            cmd.matViewName = tokens[3];
            // Find "AS" keyword
            std::string upInput = input;
            for (char& c : upInput) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            auto asPos = upInput.find(" AS ");
            if (asPos != std::string::npos) {
                cmd.matViewSql = trim(input.substr(asPos + 4));
                cmd.type = CommandType::CREATE_MATERIALIZED_VIEW;
            } else {
                cmd.type = CommandType::UNKNOWN;
            }

        // ── Phase 72: REFRESH MATERIALIZED VIEW name ─────────────
        } else if (kw0 == "REFRESH" && kw1 == "MATERIALIZED" && tokens.size() >= 4 &&
                   toUpper(tokens[2]) == "VIEW") {
            cmd.matViewName = tokens[3];
            cmd.type = CommandType::REFRESH_MATERIALIZED_VIEW;

        // ── Phase 72: DROP MATERIALIZED VIEW name ─────────────────
        } else if (kw0 == "DROP" && kw1 == "MATERIALIZED" && tokens.size() >= 4 &&
                   toUpper(tokens[2]) == "VIEW") {
            cmd.matViewName = tokens[3];
            cmd.type = CommandType::DROP_MATERIALIZED_VIEW;

        // ── Phase 75: CREATE POLICY name ON table FOR cmd TO user USING (expr) ──
        } else if (kw0 == "CREATE" && kw1 == "POLICY") {
            cmd.type = CommandType::CREATE_POLICY;
            if (tokens.size() > 2) cmd.policyName = tokens[2];
            // tokens = tokenize(beforeParen) — parens already stripped, so iterate
            // all tokens for ON/FOR/TO; USING expr extracted from raw input below.
            for (size_t i = 3; i < tokens.size(); ++i) {
                std::string t = toUpper(tokens[i]);
                if (t == "ON" && i + 1 < tokens.size())
                    cmd.tableName = tokens[i + 1];
                else if (t == "FOR" && i + 1 < tokens.size())
                    cmd.policyCommand = toUpper(tokens[i + 1]);
                else if (t == "TO" && i + 1 < tokens.size())
                    cmd.policyUser = tokens[i + 1];
            }
            // Extract USING expression from raw input using paren-depth matching
            {
                std::string upIn = input;
                for (auto& c : upIn) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                auto upos = upIn.find("USING");
                if (upos != std::string::npos) {
                    std::string after = input.substr(upos + 5);
                    // skip whitespace
                    size_t start = 0;
                    while (start < after.size() && after[start] == ' ') ++start;
                    if (start < after.size() && after[start] == '(') {
                        // paren-depth match
                        int depth = 0; size_t end = start;
                        for (size_t k = start; k < after.size(); ++k) {
                            if (after[k] == '(') ++depth;
                            else if (after[k] == ')') { --depth; if (depth == 0) { end = k; break; } }
                        }
                        cmd.policyUsingExpr = after.substr(start + 1, end - start - 1);
                    } else {
                        cmd.policyUsingExpr = after.substr(start);
                    }
                    while (!cmd.policyUsingExpr.empty() && cmd.policyUsingExpr.front() == ' ')
                        cmd.policyUsingExpr.erase(cmd.policyUsingExpr.begin());
                    while (!cmd.policyUsingExpr.empty() && cmd.policyUsingExpr.back() == ' ')
                        cmd.policyUsingExpr.pop_back();
                }
            }

        // ── Phase 75: DROP POLICY name ON table ──────────────────
        } else if (kw0 == "DROP" && kw1 == "POLICY") {
            cmd.type = CommandType::DROP_POLICY;
            if (tokens.size() > 2) cmd.policyName = tokens[2];
            for (size_t i = 3; i < tokens.size(); ++i)
                if (toUpper(tokens[i]) == "ON" && i + 1 < tokens.size())
                    cmd.tableName = tokens[i + 1];

        // ── Phase 75: SHOW POLICIES ON table ─────────────────────
        } else if (kw0 == "SHOW" && kw1 == "POLICIES") {
            cmd.type = CommandType::SHOW_POLICIES_ON;
            for (size_t i = 2; i < tokens.size(); ++i)
                if (toUpper(tokens[i]) == "ON" && i + 1 < tokens.size())
                    cmd.tableName = tokens[i + 1];

        // ── Phase 77: SET PARALLEL_THRESHOLD = N ─────────────────
        } else if (kw0 == "SET" && kw1 == "PARALLEL_THRESHOLD") {
            cmd.type = CommandType::SET_PARALLEL_THRESHOLD;
            {
                std::string val = tokens.size() >= 3 ? tokens[2] : "";
                if (val == "=" && tokens.size() >= 4) val = tokens[3];
                if (!val.empty()) cmd.values.push_back(val);
            }

        // ── Phase 77: SET MAX_PARALLEL_WORKERS = N ───────────────
        } else if (kw0 == "SET" && kw1 == "MAX_PARALLEL_WORKERS") {
            cmd.type = CommandType::SET_MAX_PARALLEL_WORKERS;
            {
                std::string val = tokens.size() >= 3 ? tokens[2] : "";
                if (val == "=" && tokens.size() >= 4) val = tokens[3];
                if (!val.empty()) cmd.values.push_back(val);
            }

        // ── Phase 76: LISTEN channel ──────────────────────────────
        } else if (kw0 == "LISTEN") {
            cmd.type = CommandType::LISTEN;
            if (tokens.size() > 1) {
                cmd.channelName = tokens[1];
                // strip trailing semicolon or comma (e.g. "LISTEN orders;")
                while (!cmd.channelName.empty() &&
                       (cmd.channelName.back() == ';' || cmd.channelName.back() == ','))
                    cmd.channelName.pop_back();
            }

        // ── Phase 76: UNLISTEN channel | UNLISTEN * ───────────────
        } else if (kw0 == "UNLISTEN") {
            cmd.type = CommandType::UNLISTEN;
            if (tokens.size() > 1) {
                cmd.channelName = tokens[1];
                while (!cmd.channelName.empty() &&
                       (cmd.channelName.back() == ';' || cmd.channelName.back() == ','))
                    cmd.channelName.pop_back();
            }

        // ── Phase 76: NOTIFY channel [, 'payload'] ────────────────
        } else if (kw0 == "NOTIFY") {
            cmd.type = CommandType::NOTIFY;
            if (tokens.size() > 1) {
                cmd.channelName = tokens[1];
                // strip trailing comma (e.g. "NOTIFY orders, 'msg'")
                while (!cmd.channelName.empty() &&
                       (cmd.channelName.back() == ';' || cmd.channelName.back() == ','))
                    cmd.channelName.pop_back();
            }
            // Optional payload after comma: NOTIFY chan, 'msg'
            {
                auto commaPos = input.find(',');
                if (commaPos != std::string::npos) {
                    std::string payStr = input.substr(commaPos + 1);
                    while (!payStr.empty() && payStr.front() == ' ') payStr.erase(payStr.begin());
                    while (!payStr.empty() && payStr.back()  == ' ') payStr.pop_back();
                    if (payStr.size() >= 2 && payStr.front() == '\'' && payStr.back() == '\'')
                        payStr = payStr.substr(1, payStr.size() - 2);
                    cmd.notifyPayload = payStr;
                }
            }

        // ── Phase 73: SHOW BUFFER POOL STATUS ────────────────────
        } else if (kw0 == "SHOW" && kw1 == "BUFFER" && tokens.size() >= 4 &&
                   toUpper(tokens[2]) == "POOL" && toUpper(tokens[3]) == "STATUS") {
            cmd.type = CommandType::SHOW_BUFFER_POOL_STATUS;

        // ── Phase 73: FLUSH BUFFER POOL ──────────────────────────
        } else if (kw0 == "FLUSH" && kw1 == "BUFFER" && tokens.size() >= 3 &&
                   toUpper(tokens[2]) == "POOL") {
            cmd.type = CommandType::FLUSH_BUFFER_POOL;

        // ── Phase 73: SET BUFFER_POOL_SIZE = N ───────────────────
        } else if (kw0 == "SET" && kw1 == "BUFFER_POOL_SIZE" && tokens.size() >= 3) {
            // tokens: SET BUFFER_POOL_SIZE = 256  OR  SET BUFFER_POOL_SIZE 256
            std::string valTok = tokens[2];
            if (valTok == "=" && tokens.size() >= 4) valTok = tokens[3];
            cmd.type = CommandType::SET_BUFFER_POOL_SIZE;
            cmd.values.push_back(valTok);

        // ── Phase 71: SET TRANSACTION ISOLATION LEVEL ────────────
        // SET TRANSACTION ISOLATION LEVEL READ COMMITTED
        // SET TRANSACTION ISOLATION LEVEL REPEATABLE READ
        // SET TRANSACTION ISOLATION LEVEL SERIALIZABLE
        } else if (kw0 == "SET" && tokens.size() >= 5 &&
                   toUpper(tokens[1]) == "TRANSACTION" &&
                   toUpper(tokens[2]) == "ISOLATION" &&
                   toUpper(tokens[3]) == "LEVEL") {
            cmd.type = CommandType::SET_TRANSACTION_ISOLATION;
            // collect remaining tokens as isolation level
            std::string lvl;
            for (size_t i = 4; i < tokens.size(); ++i) {
                if (!lvl.empty()) lvl += " ";
                lvl += toUpper(tokens[i]);
            }
            cmd.isolationLevel = lvl;

        // ── STATUS ───────────────────────────────────────────────
        } else if (kw0 == "STATUS") {
            cmd.type = CommandType::STATUS;

        // ── CREATE VIEW name AS SELECT ... ───────────────────────
        } else if (kw0 == "CREATE" && kw1 == "VIEW") {
            cmd.type = CommandType::CREATE_VIEW;
            if (tokens.size() >= 5 && toUpper(tokens[3]) == "AS") {
                cmd.tableName = tokens[2];
                // viewSql: Rohstring nach " AS " extrahieren (case-insensitive)
                std::string rawUp = input;
                for (char& c : rawUp) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                auto asPos = rawUp.find(" AS ");
                if (asPos != std::string::npos)
                    cmd.viewSql = trim(input.substr(asPos + 4));
                else
                    cmd.type = CommandType::UNKNOWN;
            } else { cmd.type = CommandType::UNKNOWN; }

        // ── DROP VIEW ─────────────────────────────────────────────
        } else if (kw0 == "DROP" && kw1 == "VIEW") {
            cmd.type = CommandType::DROP_VIEW;
            if (tokens.size() >= 3) cmd.tableName = tokens[2];
            else cmd.type = CommandType::UNKNOWN;

        // ── TRUNCATE TABLE ───────────────────────────────────────
        } else if (kw0 == "TRUNCATE" && kw1 == "TABLE") {
            cmd.type = CommandType::TRUNCATE;
            if (tokens.size() >= 3) cmd.tableName = tokens[2];
            else cmd.type = CommandType::UNKNOWN;

        // ── Phase 54A: CLEAR CACHE ───────────────────────────────
        } else if (kw0 == "CLEAR" && kw1 == "CACHE") {
            cmd.type = CommandType::CLEAR_CACHE;

        // ── Phase 54A: SET CACHE ON / SET CACHE OFF ──────────────
        } else if (kw0 == "SET" && kw1 == "CACHE") {
            cmd.type = CommandType::SET_CACHE;
            if (tokens.size() >= 3) cmd.cacheEnabled = toUpper(tokens[2]);
            else cmd.type = CommandType::UNKNOWN;

        // ── Phase 57: BACKUP DATABASE TO 'file' ──────────────────
        } else if (kw0 == "BACKUP" && kw1 == "DATABASE") {
            cmd.type = CommandType::BACKUP_DATABASE;
            if (tokens.size() >= 4 && toUpper(tokens[2]) == "TO") {
                std::string fp = tokens[3];
                if (fp.size() >= 2 && fp.front() == '\'' && fp.back() == '\'')
                    fp = fp.substr(1, fp.size() - 2);
                cmd.backupFile = fp;
            }
            // leerer backupFile → auto-generierter Name

        // ── Phase 57: BACKUP TABLE name TO 'file' ────────────────
        } else if (kw0 == "BACKUP" && kw1 == "TABLE") {
            cmd.type = CommandType::BACKUP_TABLE;
            if (tokens.size() >= 3) cmd.tableName = tokens[2];
            if (tokens.size() >= 5 && toUpper(tokens[3]) == "TO") {
                std::string fp = tokens[4];
                if (fp.size() >= 2 && fp.front() == '\'' && fp.back() == '\'')
                    fp = fp.substr(1, fp.size() - 2);
                cmd.backupFile = fp;
            }

        // ── Phase 57: RESTORE DATABASE FROM 'file' ───────────────
        } else if (kw0 == "RESTORE" && kw1 == "DATABASE") {
            cmd.type = CommandType::RESTORE_DATABASE;
            if (tokens.size() >= 4 && toUpper(tokens[2]) == "FROM") {
                std::string fp = tokens[3];
                if (fp.size() >= 2 && fp.front() == '\'' && fp.back() == '\'')
                    fp = fp.substr(1, fp.size() - 2);
                cmd.backupFile = fp;
            } else cmd.type = CommandType::UNKNOWN;

        // ── Phase 58: BENCHMARK n SQL ─────────────────────────────
        } else if (kw0 == "BENCHMARK") {
            if (tokens.size() >= 3) {
                int iter = 0;
                bool ok = true;
                try { iter = std::stoi(tokens[1]); }
                catch (...) { ok = false; }
                if (ok && iter > 0) {
                    // SQL-Teil aus Originalstring extrahieren
                    // Format: BENCHMARK <n> <sql...>
                    size_t p = 0;
                    // "BENCHMARK" überspringen
                    while (p < input.size() && !std::isspace((unsigned char)input[p])) ++p;
                    while (p < input.size() &&  std::isspace((unsigned char)input[p])) ++p;
                    // Zahl überspringen
                    while (p < input.size() && !std::isspace((unsigned char)input[p])) ++p;
                    while (p < input.size() &&  std::isspace((unsigned char)input[p])) ++p;
                    if (p < input.size()) {
                        cmd.type          = CommandType::BENCHMARK;
                        cmd.benchmarkIter = iter;
                        cmd.benchmarkSql  = input.substr(p);
                    } else {
                        cmd.type = CommandType::UNKNOWN;
                    }
                } else {
                    cmd.type = CommandType::UNKNOWN;
                }
            } else {
                cmd.type = CommandType::UNKNOWN;
            }

        // ── Phase 45: PREPARE name AS sql ────────────────────────
        } else if (kw0 == "PREPARE") {
            cmd.type = CommandType::PREPARE_STMT;
            // Find " AS " in input (case-insensitive) to split name from sql
            std::string upperInput = input;
            for (char& c : upperInput)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            size_t asPos = upperInput.find(" AS ");
            if (asPos != std::string::npos) {
                // name is between "PREPARE " and " AS "
                cmd.preparedName = input.substr(8, asPos - 8);
                while (!cmd.preparedName.empty() && cmd.preparedName.back() == ' ')
                    cmd.preparedName.pop_back();
                while (!cmd.preparedName.empty() && cmd.preparedName.front() == ' ')
                    cmd.preparedName = cmd.preparedName.substr(1);
                // SQL is everything after " AS "
                cmd.preparedSql = input.substr(asPos + 4);
                while (!cmd.preparedSql.empty() && cmd.preparedSql.front() == ' ')
                    cmd.preparedSql = cmd.preparedSql.substr(1);
            } else {
                // fallback: use token[1] as name, no sql
                if (tokens.size() >= 2) cmd.preparedName = tokens[1];
            }

        // ── Phase 45: EXECUTE name(args) ─────────────────────────
        } else if (kw0 == "EXECUTE") {
            cmd.type = CommandType::EXECUTE_STMT;
            // Parse: EXECUTE name(args) — use raw input
            // Skip "EXECUTE " prefix (8 chars) but use find for safety
            size_t s = 0;
            while (s < input.size() && (input[s] == ' ' || input[s] == '\t')) ++s;
            s += 7; // length of "EXECUTE"
            while (s < input.size() && (input[s] == ' ' || input[s] == '\t')) ++s;
            std::string rest = input.substr(s);
            size_t parenP = rest.find('(');
            if (parenP == std::string::npos) {
                cmd.preparedName = rest;
                while (!cmd.preparedName.empty() && cmd.preparedName.back() == ' ')
                    cmd.preparedName.pop_back();
            } else {
                cmd.preparedName = rest.substr(0, parenP);
                while (!cmd.preparedName.empty() && cmd.preparedName.back() == ' ')
                    cmd.preparedName.pop_back();
                size_t closeP = rest.rfind(')');
                std::string argsStr = rest.substr(parenP + 1,
                    closeP != std::string::npos ? closeP - parenP - 1 : std::string::npos);
                // Split args by comma
                std::string cur;
                for (char c : argsStr) {
                    if (c == ',') {
                        while (!cur.empty() && cur.front() == ' ') cur = cur.substr(1);
                        while (!cur.empty() && cur.back() == ' ') cur.pop_back();
                        if (!cur.empty()) cmd.execArgs.push_back(cur);
                        cur = "";
                    } else cur += c;
                }
                while (!cur.empty() && cur.front() == ' ') cur = cur.substr(1);
                while (!cur.empty() && cur.back() == ' ') cur.pop_back();
                if (!cur.empty()) cmd.execArgs.push_back(cur);
            }

        // ── Phase 45: DEALLOCATE PREPARE name ────────────────────
        } else if (kw0 == "DEALLOCATE" && kw1 == "PREPARE") {
            cmd.type = CommandType::DEALLOCATE_STMT;
            if (tokens.size() >= 3) cmd.preparedName = tokens[2];
            else cmd.type = CommandType::UNKNOWN;

        // ── Phase 46: CREATE USER ─────────────────────────────────
        } else if (kw0 == "CREATE" && kw1 == "USER") {
            cmd.type = CommandType::CREATE_USER;
            // Tokens: CREATE USER name IDENTIFIED BY 'password'
            auto toks46 = tokenizeFull(input);
            if (toks46.size() >= 3) cmd.userName = toks46[2];
            for (size_t i = 3; i < toks46.size(); ++i)
                if (toUpper(toks46[i]) == "BY" && i + 1 < toks46.size()) {
                    std::string pw = toks46[i + 1];
                    if (pw.size() >= 2 && pw.front() == '\'' && pw.back() == '\'')
                        pw = pw.substr(1, pw.size() - 2);
                    cmd.userPassword = pw;
                    break;
                }

        // ── Phase 46: DROP USER ────────────────────────────────────
        } else if (kw0 == "DROP" && kw1 == "USER") {
            cmd.type = CommandType::DROP_USER;
            if (tokens.size() >= 3) cmd.userName = tokens[2];
            else cmd.type = CommandType::UNKNOWN;

        // ── Phase 46: GRANT ────────────────────────────────────────
        } else if (kw0 == "GRANT") {
            cmd.type = CommandType::GRANT_PRIV;
            {
                auto toks46 = tokenizeFull(input);
                size_t i = 1;
                // Collect privs until ON
                while (i < toks46.size() && toUpper(toks46[i]) != "ON") {
                    if (toks46[i] != ",") cmd.grantPrivs.push_back(toUpper(toks46[i]));
                    ++i;
                }
                ++i; // skip ON
                if (i < toks46.size()) cmd.grantTable = toks46[i++];
                // skip TO
                if (i < toks46.size() && toUpper(toks46[i]) == "TO") ++i;
                if (i < toks46.size()) cmd.grantTargetUser = toks46[i];
            }

        // ── Phase 46: REVOKE ───────────────────────────────────────
        } else if (kw0 == "REVOKE") {
            cmd.type = CommandType::REVOKE_PRIV;
            {
                auto toks46 = tokenizeFull(input);
                size_t i = 1;
                while (i < toks46.size() && toUpper(toks46[i]) != "ON") {
                    if (toks46[i] != ",") cmd.grantPrivs.push_back(toUpper(toks46[i]));
                    ++i;
                }
                ++i; // skip ON
                if (i < toks46.size()) cmd.grantTable = toks46[i++];
                // skip FROM
                if (i < toks46.size() && toUpper(toks46[i]) == "FROM") ++i;
                if (i < toks46.size()) cmd.grantTargetUser = toks46[i];
            }

        // ── Phase 46: CONNECT ──────────────────────────────────────
        // Supports: CONNECT user pwd
        //       and CONNECT USER user PASSWORD pwd
        } else if (kw0 == "CONNECT") {
            cmd.type = CommandType::CONNECT_USER;
            if (tokens.size() >= 2 && toUpper(tokens[1]) == "USER") {
                // CONNECT USER alice PASSWORD 'pwd'
                if (tokens.size() >= 3) cmd.userName = tokens[2];
                // Find PASSWORD keyword
                for (size_t i = 3; i < tokens.size(); ++i) {
                    if (toUpper(tokens[i]) == "PASSWORD" && i + 1 < tokens.size()) {
                        std::string pw = tokens[i + 1];
                        // strip surrounding quotes
                        if (pw.size() >= 2 && pw.front() == '\'' && pw.back() == '\'')
                            pw = pw.substr(1, pw.size() - 2);
                        cmd.userPassword = pw;
                        break;
                    }
                }
            } else {
                if (tokens.size() >= 2) cmd.userName = tokens[1];
                if (tokens.size() >= 3) cmd.userPassword = tokens[2];
            }

        // ── Phase 46: DISCONNECT ───────────────────────────────────
        } else if (kw0 == "DISCONNECT") {
            cmd.type = CommandType::DISCONNECT_USER;

        } else if (kw0 == "DESCRIBE") {
            cmd.type = CommandType::DESCRIBE;
            if (tokens.size() >= 2) cmd.tableName = tokens[1];
            else cmd.type = CommandType::UNKNOWN;

        // ── Phase 60: LOAD DATA INFILE 'file' INTO TABLE name ───
        // [SEPARATOR ','] [SKIP HEADER]
        } else if (kw0 == "LOAD" && kw1 == "DATA") {
            cmd.type         = CommandType::LOAD_DATA;
            cmd.csvSeparator = ","; // default
            // Build upper-case version for keyword scanning
            std::string upInp;
            for (unsigned char c : input) upInp += static_cast<char>(std::toupper(c));
            // Extract quoted file path after INFILE
            auto infileKw = upInp.find("INFILE");
            if (infileKw != std::string::npos) {
                std::string afterInfile = input.substr(infileKw + 6);
                for (char q : {'\'', '"'}) {
                    auto q1 = afterInfile.find(q);
                    if (q1 != std::string::npos) {
                        auto q2 = afterInfile.find(q, q1 + 1);
                        if (q2 != std::string::npos) {
                            cmd.csvFile = afterInfile.substr(q1 + 1, q2 - q1 - 1);
                            break;
                        }
                    }
                }
            }
            // Table name: word after "TABLE"
            auto tableKw = upInp.find(" TABLE ");
            if (tableKw != std::string::npos) {
                size_t ts = tableKw + 7;
                while (ts < input.size() && input[ts] == ' ') ++ts;
                size_t te = ts;
                while (te < input.size() && input[te] != ' ' && input[te] != '\n') ++te;
                cmd.tableName = input.substr(ts, te - ts);
            }
            // SEPARATOR value
            auto sepKw = upInp.find("SEPARATOR");
            if (sepKw != std::string::npos) {
                std::string afterSep = input.substr(sepKw + 9);
                for (char q : {'\'', '"'}) {
                    auto q1 = afterSep.find(q);
                    if (q1 != std::string::npos) {
                        auto q2 = afterSep.find(q, q1 + 1);
                        if (q2 != std::string::npos) {
                            cmd.csvSeparator = afterSep.substr(q1 + 1, q2 - q1 - 1);
                            if (cmd.csvSeparator.empty()) cmd.csvSeparator = ",";
                            break;
                        }
                    }
                }
            }
            // SKIP HEADER
            if (upInp.find("SKIP") != std::string::npos &&
                upInp.find("HEADER") != std::string::npos)
                cmd.csvSkipHeader = true;

            if (cmd.csvFile.empty() || cmd.tableName.empty())
                cmd.type = CommandType::UNKNOWN;

        // ── Phase 61: CREATE EVENT name ON SCHEDULE ... DO ... ───
        } else if (kw0 == "CREATE" && kw1 == "EVENT") {
            cmd.type = CommandType::CREATE_EVENT;
            {
                // Find " DO " in uppercase to split schedule from sql
                std::string upInp;
                for (unsigned char c : input)
                    upInp += static_cast<char>(std::toupper(c));
                auto doPos = upInp.find(" DO ");
                if (doPos != std::string::npos) {
                    std::string sqlPart = input.substr(doPos + 4);
                    while (!sqlPart.empty() && (sqlPart.front()==' '||sqlPart.front()=='\t'))
                        sqlPart = sqlPart.substr(1);
                    cmd.eventSql = sqlPart;
                }
                // Parse schedule from the prefix (before DO)
                std::string prefix = (doPos != std::string::npos)
                    ? input.substr(0, doPos) : input;
                auto ft = tokenizeFull(prefix);
                size_t i = 2; // skip CREATE EVENT
                if (i < ft.size()) cmd.eventName = ft[i++];
                // skip ON SCHEDULE
                if (i < ft.size() && toUpper(ft[i]) == "ON")       ++i;
                if (i < ft.size() && toUpper(ft[i]) == "SCHEDULE") ++i;
                if (i < ft.size()) {
                    std::string schedType = toUpper(ft[i]);
                    if (schedType == "EVERY") {
                        cmd.eventRecurring = true;
                        ++i;
                        if (i < ft.size()) {
                            try { cmd.eventIntervalN = std::stoll(ft[i]); }
                            catch (...) { cmd.eventIntervalN = 1; }
                            ++i;
                        }
                        if (i < ft.size()) {
                            cmd.eventIntervalUnit = toUpper(ft[i]); ++i;
                        }
                        // Optional: AT 'HH:MM:SS'
                        if (i < ft.size() && toUpper(ft[i]) == "AT") {
                            ++i;
                            if (i < ft.size()) {
                                cmd.eventHasAt = true;
                                cmd.eventAtTime = ft[i];
                                // strip surrounding single quotes
                                if (cmd.eventAtTime.size() >= 2 &&
                                    cmd.eventAtTime.front() == '\'')
                                    cmd.eventAtTime = cmd.eventAtTime.substr(
                                        1, cmd.eventAtTime.size() - 2);
                            }
                        }
                    } else if (schedType == "AT") {
                        cmd.eventRecurring = false;
                        ++i;
                        if (i < ft.size()) {
                            cmd.eventAtTime = ft[i];
                            if (cmd.eventAtTime.size() >= 2 &&
                                cmd.eventAtTime.front() == '\'')
                                cmd.eventAtTime = cmd.eventAtTime.substr(
                                    1, cmd.eventAtTime.size() - 2);
                        }
                    }
                }
                if (cmd.eventName.empty() || cmd.eventSql.empty())
                    cmd.type = CommandType::UNKNOWN;
            }

        // ── Phase 61: DROP EVENT name ─────────────────────────────
        } else if (kw0 == "DROP" && kw1 == "EVENT") {
            cmd.type = CommandType::DROP_EVENT;
            if (tokens.size() >= 3) cmd.eventName = tokens[2];
            else cmd.type = CommandType::UNKNOWN;

        // ── Phase 61: ALTER EVENT name ENABLE/DISABLE ─────────────
        } else if (kw0 == "ALTER" && kw1 == "EVENT") {
            cmd.type = CommandType::ALTER_EVENT;
            if (tokens.size() >= 4) {
                cmd.eventName = tokens[2];
                std::string action = toUpper(tokens[3]);
                if (action == "ENABLE")        cmd.eventEnabled = true;
                else if (action == "DISABLE")  cmd.eventEnabled = false;
                else cmd.type = CommandType::UNKNOWN;
            } else cmd.type = CommandType::UNKNOWN;

        // ── Phase 61: SET EVENT_SCHEDULER = ON/OFF ────────────────
        } else if (kw0 == "SET" && toUpper(kw1) == "EVENT_SCHEDULER") {
            cmd.type = CommandType::SET_EVENT_SCHEDULER;
            // tokens: SET EVENT_SCHEDULER = ON  or  SET EVENT_SCHEDULER ON
            std::string val;
            for (size_t si = 2; si < tokens.size(); ++si) {
                std::string t = toUpper(tokens[si]);
                if (t == "ON" || t == "OFF") { val = t; break; }
            }
            if (val.empty()) cmd.type = CommandType::UNKNOWN;
            else cmd.eventSchedulerOn = (val == "ON");

        // ── Phase 59: STOP SLAVE / START SLAVE ──────────────────
        } else if (kw0 == "STOP" && kw1 == "SLAVE") {
            cmd.type = CommandType::STOP_SLAVE;
        } else if (kw0 == "START" && kw1 == "SLAVE") {
            cmd.type = CommandType::START_SLAVE;

        } else if (kw0 == "HELP") { cmd.type = CommandType::HELP; }
        else if  (kw0 == "EXIT") { cmd.type = CommandType::EXIT; }
        else                     { cmd.type = CommandType::UNKNOWN; }

        return cmd;
    }

private:
    // ── Phase 51: Qualified name helper ──────────────────────
    // Reads a possibly schema-qualified name from tokenizeFull tokens.
    // If tokens[i] is "name" and tokens[i+1] is "." and tokens[i+2] is "table",
    // collapses them into "name.table" and advances i by 3.
    // Otherwise just returns tokens[i] and advances i by 1.
    static std::string parseQualifiedName(const std::vector<std::string>& toks,
                                          size_t& i) {
        if (i >= toks.size()) return "";
        std::string name = toks[i++];
        // Check for dot-qualified suffix (schema.table or table.col pattern)
        if (i < toks.size() && toks[i] == "." && i + 1 < toks.size()) {
            // Only collapse if the next part looks like an identifier (not a keyword/operator)
            ++i; // skip "."
            name += "." + toks[i++];
        }
        return name;
    }

    // ── Phase 22: Multi-Column SET-Zuweisung (UPDATE) ────────
    // Parst beliebig viele "col=val" Paare (Komma-getrennt) bis end.
    // Setzt updateCols/updateVals und backward-compat setColumn/setValue.
    static void parseMultiAssignment(const std::vector<std::string>& tokens,
                                      size_t start, size_t end,
                                      ParsedCommand& cmd) {
        for (size_t i = start; i < end; ) {
            std::string tok = tokens[i];
            // Trailing comma entfernen (z.B. "name=Alice,")
            if (!tok.empty() && tok.back() == ',') tok.pop_back();
            if (tok.empty()) { ++i; continue; }

            std::string col, op, val;
            if (extractColOpVal(tok, col, op, val) && op == "=") {
                // "col=val" in einem Token
                if (!val.empty() && val.back() == ',') val.pop_back();
                cmd.updateCols.push_back(col);
                cmd.updateVals.push_back(val);
                ++i;
            } else if (i + 2 < end && tokens[i + 1] == "=") {
                // "col = val [op val2 ...]" — collect until next col=val or end
                // Collect multi-token value expression (e.g. "gehalt + 1000")
                i += 2;  // skip col and =
                std::string v;
                // Collect tokens until next ',' or a token that looks like col=val or end
                while (i < end && tokens[i] != ",") {
                    // Check if this looks like the start of next assignment: col =
                    // (i.e., next token is '=' or current token contains '=' as non-first char)
                    if (i + 1 < end && tokens[i + 1] == "=") break;
                    // single-token col=val
                    {
                        std::string c2, o2, v2;
                        if (extractColOpVal(tokens[i], c2, o2, v2) && o2 == "=") break;
                    }
                    if (!v.empty()) v += " ";
                    std::string vt = tokens[i];
                    if (!vt.empty() && vt.back() == ',') { v += vt.substr(0, vt.size()-1); ++i; break; }
                    v += vt;
                    ++i;
                }
                if (!v.empty() && v.back() == ',') v.pop_back();
                cmd.updateCols.push_back(tok);
                cmd.updateVals.push_back(v);
                // Optional nachfolgendes Komma-Token überspringen
                if (i < end && tokens[i] == ",") ++i;
            } else {
                ++i;  // nicht parsebar, überspringen
            }
        }
        // Backward-compat: erstes Paar als setColumn/setValue setzen
        if (!cmd.updateCols.empty()) {
            cmd.setColumn = cmd.updateCols[0];
            cmd.setValue  = cmd.updateVals[0];
        }
    }

    // ── SET-Zuweisung (UPDATE) — Einzel-Fallback ─────────────

    static void parseAssignment(const std::vector<std::string>& tokens,
                                size_t start, ParsedCommand& cmd) {
        if (start >= tokens.size()) return;
        auto eq = tokens[start].find('=');
        if (eq != std::string::npos) {
            cmd.setColumn = tokens[start].substr(0, eq);
            cmd.setValue  = tokens[start].substr(eq + 1);
            return;
        }
        if (start + 2 < tokens.size() && tokens[start + 1] == "=") {
            cmd.setColumn = tokens[start];
            cmd.setValue  = tokens[start + 2];
        }
    }

    // ── WHERE-Token col/op/val aufsplitten ────────────────────

    static bool extractColOpVal(const std::string& token,
                                 std::string& col, std::string& op, std::string& val) {
        for (const std::string& cand :
             std::vector<std::string>{">=", "<=", "!="}) {
            auto pos = token.find(cand);
            if (pos != std::string::npos) {
                col = token.substr(0, pos); op = cand;
                val = token.substr(pos + 2);
                return !col.empty();
            }
        }
        for (char c : {'=', '<', '>'}) {
            auto pos = token.find(c);
            if (pos != std::string::npos) {
                col = token.substr(0, pos); op = std::string(1, c);
                val = token.substr(pos + 1);
                return !col.empty();
            }
        }
        return false;
    }

    // ── WHERE col op val [AND|OR ...] ─────────────────────────
    // Unterstützt: =, !=, <, >, <=, >=, LIKE,
    //              IS NULL, IS NOT NULL,
    //              IN (v1, v2, ...) / IN (SELECT col FROM tbl [WHERE ...])
    //              NOT IN (...)

    static void parseWhere(const std::vector<std::string>& tokens,
                           size_t start, ParsedCommand& cmd) {
        if (start >= tokens.size() || toUpper(tokens[start]) != "WHERE") return;

        size_t i = start + 1;
        cmd.whereLogic = "AND";

        while (i < tokens.size()) {
            std::string u = toUpper(tokens[i]);

            if (u == "ORDER" || u == "LIMIT" || u == "GROUP") break;
            if (u == "AND") { cmd.whereLogic = "AND"; ++i; continue; }
            if (u == "OR")  { cmd.whereLogic = "OR";  ++i; continue; }

            WhereCondition cond;
            bool parsed = false;

            // IS NULL / IS NOT NULL
            if (i + 2 < tokens.size() &&
                toUpper(tokens[i + 1]) == "IS" &&
                toUpper(tokens[i + 2]) == "NULL") {
                cond = {tokens[i], "IS NULL", ""};
                i += 3; parsed = true;

            } else if (i + 3 < tokens.size() &&
                       toUpper(tokens[i + 1]) == "IS" &&
                       toUpper(tokens[i + 2]) == "NOT" &&
                       toUpper(tokens[i + 3]) == "NULL") {
                cond = {tokens[i], "IS NOT NULL", ""};
                i += 4; parsed = true;

            // NOT IN (...)
            } else if (i + 1 < tokens.size() &&
                       toUpper(tokens[i + 1]) == "NOT" &&
                       i + 2 < tokens.size() &&
                       toUpper(tokens[i + 2]) == "IN") {
                cond.col = tokens[i];
                cond.op  = "NOT IN";
                i += 3;
                parseInList(tokens, i, cond, cmd);
                parsed = true;

            // Phase 64: NOT REGEXP / NOT RLIKE
            } else if (i + 3 < tokens.size() &&
                       toUpper(tokens[i + 1]) == "NOT" &&
                       (toUpper(tokens[i + 2]) == "REGEXP" ||
                        toUpper(tokens[i + 2]) == "RLIKE")) {
                cond = {tokens[i], "NOT REGEXP", tokens[i + 3]};
                i += 4; parsed = true;

            // IN (...)
            } else if (i + 1 < tokens.size() &&
                       toUpper(tokens[i + 1]) == "IN") {
                cond.col = tokens[i];
                cond.op  = "IN";
                i += 2;
                parseInList(tokens, i, cond, cmd);
                parsed = true;

            } else if (extractColOpVal(tokens[i], cond.col, cond.op, cond.val)) {
                ++i; parsed = true;

            } else if (i + 2 < tokens.size() && toUpper(tokens[i + 1]) == "LIKE") {
                cond = {tokens[i], "LIKE", tokens[i + 2]};
                i += 3; parsed = true;

            // Phase 64: REGEXP / RLIKE
            } else if (i + 2 < tokens.size() &&
                       (toUpper(tokens[i + 1]) == "REGEXP" ||
                        toUpper(tokens[i + 1]) == "RLIKE")) {
                cond = {tokens[i], "REGEXP", tokens[i + 2]};
                i += 3; parsed = true;

            } else if (i + 2 < tokens.size()) {
                const std::string& opStr = tokens[i + 1];
                for (const auto& o :
                     std::vector<std::string>{">=", "<=", "!=", "=", "<", ">"}) {
                    if (opStr == o) {
                        cond = {tokens[i], o, tokens[i + 2]};
                        i += 3; parsed = true;
                        break;
                    }
                }
            }

            if (parsed) {
                cmd.whereConds.push_back(cond);
            } else {
                break;
            }
        }

        if (!cmd.whereConds.empty()) {
            cmd.whereColumn = cmd.whereConds[0].col;
            cmd.whereOp     = cmd.whereConds[0].op;
            cmd.whereValue  = cmd.whereConds[0].val;
        }
    }

    // IN-Liste oder Subquery parsen.
    // Extrahiert den Inhalt zwischen den äußeren Klammern zeichenweise,
    // damit "(100," und "50)" korrekt verarbeitet werden.
    // Nach dem Aufruf steht i hinter der schließenden ")".
    static void parseInList(const std::vector<std::string>& tokens,
                             size_t& i, WhereCondition& cond,
                             ParsedCommand& cmd) {
        // Zeichenweise den Inhalt zwischen den äußeren ( ) sammeln
        std::string content;
        int  depth  = 0;
        bool opened = false;
        bool done   = false;

        while (i < tokens.size() && !done) {
            for (char c : tokens[i]) {
                if (!opened) {
                    if (c == '(') { opened = true; depth = 1; }
                    // alles vor '(' ignorieren
                } else {
                    if (c == '(') { ++depth; content += c; }
                    else if (c == ')') {
                        if (--depth == 0) { done = true; break; }
                        content += c;
                    } else {
                        content += c;
                    }
                }
            }
            if (!done && opened) content += ' ';
            ++i;
        }

        // Führende / abschließende Leerzeichen entfernen
        while (!content.empty() && content.front() == ' ')
            content.erase(content.begin());
        while (!content.empty() && content.back()  == ' ')
            content.pop_back();

        // Subquery? Inhalt beginnt mit SELECT
        std::string up6 = toUpper(content.substr(0, std::min(content.size(), size_t(6))));
        if (up6 == "SELECT") {
            auto subToks = tokenize(content);
            std::string subCol, subTable;
            size_t j = 1;  // SELECT überspringen
            if (j < subToks.size()) subCol   = subToks[j++];
            if (j < subToks.size() && toUpper(subToks[j]) == "FROM") ++j;
            if (j < subToks.size()) subTable = subToks[j++];

            ParsedCommand subCmd;
            if (j < subToks.size() && toUpper(subToks[j]) == "WHERE")
                parseWhere(subToks, j, subCmd);

            ParsedCommand::SubquerySpec spec;
            spec.condIdx       = cmd.whereConds.size(); // Index der noch einzufügenden cond
            spec.subTable      = subTable;
            spec.subCol        = subCol;
            spec.subWhere      = subCmd.whereConds;
            spec.subWhereLogic = subCmd.whereLogic;
            cmd.subqueries.push_back(std::move(spec));

        } else {
            // Werteliste: "100, 200, 50" oder "Ali, Max"
            for (auto& val : splitTrim(content, ',')) {
                std::string v = trim(val);
                if (!v.empty()) cond.inList.push_back(v);
            }
        }
    }

    // ── Phase 38: ORDER BY col1 [ASC|DESC], col2 [ASC|DESC], ... ──

    // Helper: strip trailing comma from a token
    static std::string stripComma(const std::string& s) {
        if (!s.empty() && s.back() == ',') return s.substr(0, s.size() - 1);
        return s;
    }

    // Helper: parse comma-separated "col [ASC|DESC]" pairs from ft[start..end)
    // Works with both tokenizeFull (explicit "," tokens) and tokenize (trailing commas)
    static void parseOrderByCols(const std::vector<std::string>& ft,
                                  size_t start, size_t end,
                                  std::vector<std::pair<std::string,bool>>& out) {
        size_t i = start;
        while (i < end) {
            if (ft[i] == ",") { ++i; continue; }
            std::string col = stripComma(ft[i++]);
            if (col.empty()) continue;
            // col itself might be "ASC" or "DESC" (when prior col had trailing comma)
            std::string cu = toUpper(col);
            if (cu == "ASC" || cu == "DESC") continue;  // skip orphaned direction keywords
            bool desc = false;
            if (i < end) {
                std::string d = toUpper(stripComma(ft[i]));
                if (d == "DESC") { desc = true; ++i; }
                else if (d == "ASC") { ++i; }
            }
            out.push_back({col, desc});
        }
    }

    static void parseOrderBy(const std::vector<std::string>& tokens,
                              size_t startIdx, ParsedCommand& cmd) {
        for (size_t i = startIdx; i < tokens.size(); ++i) {
            if (toUpper(tokens[i]) == "ORDER" &&
                i + 2 < tokens.size() &&
                toUpper(tokens[i + 1]) == "BY") {
                // Collect tokens until LIMIT or end
                size_t j = i + 2;
                size_t end = tokens.size();
                for (size_t k = j; k < tokens.size(); ++k) {
                    if (toUpper(tokens[k]) == "LIMIT") { end = k; break; }
                }
                parseOrderByCols(tokens, j, end, cmd.orderByCols);
                return;
            }
        }
    }

    // ── LIMIT N [OFFSET M] ────────────────────────────────────

    static void parseLimit(const std::vector<std::string>& tokens,
                           size_t startIdx, ParsedCommand& cmd) {
        for (size_t i = startIdx; i < tokens.size(); ++i) {
            if (toUpper(tokens[i]) == "LIMIT" && i + 1 < tokens.size()) {
                try { cmd.limit = std::stoi(tokens[i + 1]); }
                catch (...) { cmd.limit = -1; }
                // Phase 38: OFFSET
                if (i + 3 < tokens.size() && toUpper(tokens[i + 2]) == "OFFSET") {
                    try { cmd.limitOffset = std::stoi(tokens[i + 3]); }
                    catch (...) { cmd.limitOffset = 0; }
                }
                return;
            }
        }
    }

    // ── Phase 16: ALTER TABLE ────────────────────────────────────
    // Syntax:
    //   ALTER TABLE tbl ADD    COLUMN col TYPE
    //   ALTER TABLE tbl DROP   COLUMN col
    //   ALTER TABLE tbl RENAME COLUMN old TO new
    //   ALTER TABLE tbl ADD    PARTITION (...)     [Phase 62]
    //   ALTER TABLE tbl DROP   PARTITION name      [Phase 62]
    static void parseAlterTableCmd(const std::vector<std::string>& tokens,
                                    ParsedCommand& cmd,
                                    const std::string& rawInput = "") {
        cmd.type = CommandType::ALTER_TABLE;
        // tokens: [ALTER, TABLE, tblname, op, COLUMN/PARTITION, ...]
        if (tokens.size() < 4) { cmd.type = CommandType::UNKNOWN; return; }

        cmd.tableName = tokens[2];
        std::string op  = toUpper(tokens[3]);
        std::string kw4 = tokens.size() >= 5 ? toUpper(tokens[4]) : "";

        // Phase 62: ALTER TABLE t ADD PARTITION (...)
        if (op == "ADD" && kw4 == "PARTITION") {
            cmd.alterOp = "ADD_PARTITION";
            // Parse partition definition from rawInput paren content
            auto pstart = rawInput.find('(');
            if (pstart != std::string::npos) {
                auto pend = rawInput.rfind(')');
                if (pend != std::string::npos && pend > pstart) {
                    std::string pdef = trim(rawInput.substr(pstart + 1, pend - pstart - 1));
                    std::string updef = toUpper(pdef);
                    // PARTITION name VALUES LESS THAN (n)
                    auto tpd = tokenize(pdef);
                    if (tpd.size() >= 2 && toUpper(tpd[0]) == "PARTITION") {
                        cmd.partitionName = tpd[1];
                        if (updef.find("VALUES LESS THAN") != std::string::npos) {
                            // RANGE partition
                            auto vp = pdef.find('(', updef.find("VALUES LESS THAN") + 16);
                            auto vpe = pdef.rfind(')');
                            if (vp != std::string::npos && vpe != std::string::npos && vpe > vp) {
                                cmd.addRangeDef.name = tpd[1];
                                cmd.addRangeDef.limitStr = trim(pdef.substr(vp + 1, vpe - vp - 1));
                            }
                        } else if (updef.find("VALUES IN") != std::string::npos) {
                            // LIST partition
                            auto viPos = updef.find("VALUES IN");
                            auto vp = pdef.find('(', viPos + 9);
                            auto vpe = pdef.rfind(')');
                            if (vp != std::string::npos && vpe != std::string::npos && vpe > vp) {
                                cmd.addListDef.name = tpd[1];
                                std::string vals = pdef.substr(vp + 1, vpe - vp - 1);
                                for (auto& v : splitTrim(vals, ',')) {
                                    std::string sv = v;
                                    if (sv.size() >= 2 && sv.front() == '\'' && sv.back() == '\'')
                                        sv = sv.substr(1, sv.size() - 2);
                                    cmd.addListDef.values.push_back(sv);
                                }
                            }
                        }
                    }
                }
            }
        // Phase 62: ALTER TABLE t DROP PARTITION name
        } else if (op == "DROP" && kw4 == "PARTITION" && tokens.size() >= 6) {
            cmd.alterOp      = "DROP_PARTITION";
            cmd.partitionName = tokens[5];

        // Phase 75: ALTER TABLE t ENABLE/DISABLE ROW LEVEL SECURITY
        } else if (op == "ENABLE" && kw4 == "ROW" &&
                   tokens.size() >= 7 &&
                   toUpper(tokens[5]) == "LEVEL" &&
                   toUpper(tokens[6]) == "SECURITY") {
            cmd.alterOp = "ENABLE_RLS";

        } else if (op == "DISABLE" && kw4 == "ROW" &&
                   tokens.size() >= 7 &&
                   toUpper(tokens[5]) == "LEVEL" &&
                   toUpper(tokens[6]) == "SECURITY") {
            cmd.alterOp = "DISABLE_RLS";

        } else if (tokens.size() < 5) {
            cmd.type = CommandType::UNKNOWN;

        } else if (op == "ADD" && kw4 == "COLUMN" && tokens.size() >= 6) {
            cmd.alterOp      = "ADD";
            cmd.alterColName = tokens[5];
            cmd.alterColType = tokens.size() >= 7 ? toUpper(tokens[6]) : "TEXT";

        } else if (op == "DROP" && kw4 == "COLUMN" && tokens.size() >= 6) {
            cmd.alterOp      = "DROP";
            cmd.alterColName = tokens[5];

        } else if (op == "RENAME" && kw4 == "COLUMN" && tokens.size() >= 8 &&
                   toUpper(tokens[6]) == "TO") {
            cmd.alterOp      = "RENAME";
            cmd.alterColName = tokens[5];   // alter Name
            cmd.alterColNew  = tokens[7];   // neuer Name

        } else {
            cmd.type = CommandType::UNKNOWN;
        }
    }

    // ── Phase 14: IN/NOT IN — tokenizeFull-basiertes Parsen ─────

    // WHERE-Parser für tokenizeFull-Output (Parens als eigene Tokens).
    // Unterstützt alle WHERE-Operatoren inkl. IN (Werteliste + Subquery).
    static void parseWhereFromFull(const std::vector<std::string>& ft,
                                    size_t start, size_t end,
                                    ParsedCommand& cmd) {
        if (start >= end || toUpper(ft[start]) != "WHERE") return;
        size_t i = start + 1;
        cmd.whereLogic = "AND";

        while (i < end) {
            std::string u = toUpper(ft[i]);
            if (u == "AND") { cmd.whereLogic = "AND"; ++i; continue; }
            if (u == "OR")  { cmd.whereLogic = "OR";  ++i; continue; }
            if (u == "ORDER" || u == "LIMIT" || u == "GROUP") break;

            WhereCondition cond;
            bool parsed = false;

            // Phase 49: MATCH(col1, col2) AGAINST('query') in WHERE
            if (u == "MATCH") {
                cond.isMatchAgainst = true;
                ++i; // skip MATCH
                if (i < end && ft[i] == "(") ++i; // skip (
                while (i < end && ft[i] != ")") {
                    if (ft[i] != ",") cond.matchCols.push_back(ft[i]);
                    ++i;
                }
                if (i < end) ++i; // skip )
                if (i < end && toUpper(ft[i]) == "AGAINST") ++i; // skip AGAINST
                if (i < end && ft[i] == "(") ++i; // skip (
                while (i < end && ft[i] != ")") {
                    std::string qt = ft[i];
                    if (qt.size() >= 2 && qt.front() == '\'' && qt.back() == '\'')
                        qt = qt.substr(1, qt.size() - 2);
                    if (!cond.againstQuery.empty()) cond.againstQuery += " ";
                    cond.againstQuery += qt;
                    ++i;
                }
                if (i < end) ++i; // skip )
                cmd.whereConds.push_back(cond);
                continue;
            }

            // Phase 56: JSON-Funktionen als WHERE-LHS
            // Phase 40: CAST ( expr AS TYPE ) op val
            // Alle bekannten Funktionen mit ( im WHERE als isFuncLhs behandeln
            static const std::vector<std::string> FUNC_LHS_NAMES =
                {"CAST", "JSON_EXTRACT", "JSON_LENGTH", "JSON_CONTAINS",
                 "JSON_TYPE", "JSON_KEYS", "JSON_VALID",
                 "YEAR", "MONTH", "DAY", "HOUR", "MINUTE", "SECOND",
                 "DATE", "TIME", "NOW", "CURDATE", "CURTIME",
                 "UPPER", "LOWER", "LENGTH", "CONCAT", "SUBSTR", "TRIM",
                 "REPLACE", "ABS", "ROUND", "MOD", "COALESCE", "IFNULL",
                 // Phase 70: Spatial-Funktionen in WHERE
                 "ST_DISTANCE", "ST_X", "ST_Y", "ST_WITHIN", "ST_ASTEXT"};
            bool isFuncLhsCandidate = false;
            for (const auto& fn : FUNC_LHS_NAMES)
                if (u == fn) { isFuncLhsCandidate = true; break; }

            if (isFuncLhsCandidate && i + 1 < end && ft[i+1] == "(") {
                // Collect function tokens including arguments through matching )
                std::string funcExpr = u;  // function name (uppercase)
                size_t j = i + 1;
                int depth = 0;
                while (j < end) {
                    funcExpr += " " + ft[j];
                    if (ft[j] == "(") ++depth;
                    else if (ft[j] == ")") { --depth; if (depth == 0) { ++j; break; } }
                    ++j;
                }
                // j now points to op token
                cond.isFuncLhs   = true;
                cond.funcLhsExpr = funcExpr;
                cond.col         = "";  // no column name
                // Read op and val
                if (j < end) {
                    cond.op = ft[j]; ++j;
                    if (j < end) { cond.val = ft[j]; ++j; }
                }
                i = j;
                parsed = true;

            // IS NULL / IS NOT NULL
            } else if (i + 2 < end && toUpper(ft[i+1]) == "IS" && toUpper(ft[i+2]) == "NULL") {
                cond = {ft[i], "IS NULL", ""};
                i += 3; parsed = true;
            } else if (i + 3 < end && toUpper(ft[i+1]) == "IS" &&
                       toUpper(ft[i+2]) == "NOT" && toUpper(ft[i+3]) == "NULL") {
                cond = {ft[i], "IS NOT NULL", ""};
                i += 4; parsed = true;

            // NOT IN / NOT BETWEEN / NOT EXISTS
            } else if (i + 1 < end && toUpper(ft[i]) == "NOT") {
                std::string u2 = i + 1 < end ? toUpper(ft[i+1]) : "";
                if (u2 == "EXISTS") {
                    // NOT EXISTS (SELECT ...)
                    cond.col = ""; cond.op = "NOT EXISTS";
                    i += 2;
                    parseExistsFromFull(ft, i, end, cond);
                    parsed = true;
                } else if (!cond.col.empty() && u2 == "IN") {
                    // col NOT IN (...)
                    cond.op = "NOT IN";
                    i += 2;
                    parseInListFromFull(ft, i, end, cond, cmd);
                    parsed = true;
                } else if (!cond.col.empty() && u2 == "BETWEEN") {
                    // col NOT BETWEEN low AND high
                    cond.op = "NOT BETWEEN";
                    i += 2;
                    if (i < end) cond.betweenLow = ft[i++];
                    if (i < end && toUpper(ft[i]) == "AND") ++i;
                    if (i < end) cond.betweenHigh = ft[i++];
                    parsed = true;
                } else {
                    ++i; continue;  // unbekannt
                }

            // EXISTS (SELECT ...)
            } else if (u == "EXISTS") {
                cond.col = ""; cond.op = "EXISTS";
                ++i;
                parseExistsFromFull(ft, i, end, cond);
                parsed = true;

            // NOT IN (... — wird über "NOT" Branch oben abgehandelt,
            // aber zum Sicherheit: col NOT IN wenn col bereits gesetzt
            } else if (i + 2 < end && toUpper(ft[i+1]) == "NOT" &&
                       toUpper(ft[i+2]) == "IN") {
                cond.col = ft[i]; cond.op = "NOT IN";
                i += 3;
                parseInListFromFull(ft, i, end, cond, cmd);
                parsed = true;

            // col NOT BETWEEN
            } else if (i + 2 < end && toUpper(ft[i+1]) == "NOT" &&
                       toUpper(ft[i+2]) == "BETWEEN") {
                cond.col = ft[i]; cond.op = "NOT BETWEEN";
                i += 3;
                if (i < end) cond.betweenLow = ft[i++];
                if (i < end && toUpper(ft[i]) == "AND") ++i;
                if (i < end) cond.betweenHigh = ft[i++];
                parsed = true;

            // col BETWEEN low AND high
            } else if (i + 1 < end && toUpper(ft[i+1]) == "BETWEEN") {
                cond.col = ft[i]; cond.op = "BETWEEN";
                i += 2;
                if (i < end) cond.betweenLow = ft[i++];
                if (i < end && toUpper(ft[i]) == "AND") ++i;
                if (i < end) cond.betweenHigh = ft[i++];
                parsed = true;

            // IN (...)
            } else if (i + 1 < end && toUpper(ft[i+1]) == "IN") {
                cond.col = ft[i]; cond.op = "IN";
                i += 2;
                parseInListFromFull(ft, i, end, cond, cmd);
                parsed = true;

            // LIKE
            } else if (i + 2 < end && toUpper(ft[i+1]) == "LIKE") {
                cond = {ft[i], "LIKE", ft[i+2]};
                i += 3; parsed = true;

            // Standard op: col OP val (3 Tokens)
            } else if (i + 2 < end) {
                const std::string& opStr = ft[i+1];
                for (const auto& o :
                     std::vector<std::string>{">=", "<=", "!=", "=", "<", ">"}) {
                    if (opStr == o) {
                        // Phase 37: col op (SELECT ...) — skalare korrelierte Subquery
                        if (ft[i+2] == "(" && i + 3 < end && toUpper(ft[i+3]) == "SELECT") {
                            cond.col = ft[i];
                            cond.op  = o;
                            cond.isScalarSub = true;
                            i += 2;  // skip col and op, now at (
                            ++i;     // skip (
                            parseScalarSubFromFull(ft, i, end, cond.scalarSub);
                            parsed = true;
                        } else {
                            cond = {ft[i], o, ft[i+2]};
                            i += 3; parsed = true;
                        }
                        break;
                    }
                }
                // kompakter Fall "col=val"
                if (!parsed) {
                    std::string col, op, val;
                    if (extractColOpVal(ft[i], col, op, val)) {
                        cond = {col, op, val};
                        ++i; parsed = true;
                    }
                }
            } else {
                std::string col, op, val;
                if (extractColOpVal(ft[i], col, op, val)) {
                    cond = {col, op, val};
                    ++i; parsed = true;
                }
            }

            if (parsed) cmd.whereConds.push_back(cond);
            else ++i;
        }

        if (!cmd.whereConds.empty()) {
            cmd.whereColumn = cmd.whereConds[0].col;
            cmd.whereOp     = cmd.whereConds[0].op;
            cmd.whereValue  = cmd.whereConds[0].val;
        }
    }

    // ── Phase 37: Scalar Subquery aus tokenizeFull-Tokens lesen ──
    // i zeigt auf SELECT (das ( wurde bereits konsumiert).
    // Format: SELECT aggfunc(col) FROM tbl [alias] [WHERE cond [AND cond ...]] )
    static void parseScalarSubFromFull(const std::vector<std::string>& ft,
                                        size_t& i, size_t end,
                                        ScalarSubSpec& spec) {
        // SELECT überspringen
        if (i < end && toUpper(ft[i]) == "SELECT") ++i;

        // SELECT-Ausdruck: aggfunc(col) oder col oder *
        static const std::vector<std::string> AGG = {"COUNT","AVG","SUM","MIN","MAX"};
        if (i < end) {
            std::string u = toUpper(ft[i]);
            bool isAgg = false;
            for (const auto& f : AGG) if (u == f) { isAgg = true; break; }
            if (isAgg) {
                spec.aggFunc = u; ++i;
                if (i < end && ft[i] == "(") ++i;        // skip (
                if (i < end && ft[i] != ")") { spec.aggCol = ft[i]; ++i; }
                if (i < end && ft[i] == ")") ++i;        // skip )
            } else if (u != "FROM") {
                spec.aggFunc = ""; spec.aggCol = ft[i]; ++i;
            }
        }

        // FROM
        if (i < end && toUpper(ft[i]) == "FROM") ++i;

        // Tabellenname
        if (i < end && ft[i] != ")" && toUpper(ft[i]) != "WHERE") {
            spec.subTable = ft[i++];
            // Optional: Sub-Tabellen-Alias überspringen
            if (i < end && ft[i] != ")" && toUpper(ft[i]) != "WHERE" &&
                ft[i] != "," && ft[i] != "ORDER" && ft[i] != "LIMIT")
                ++i;
        }

        // WHERE
        if (i < end && toUpper(ft[i]) == "WHERE") {
            ++i;
            while (i < end && ft[i] != ")") {
                std::string u = toUpper(ft[i]);
                if (u == "AND") { spec.whereLogic = "AND"; ++i; continue; }
                if (u == "OR")  { spec.whereLogic = "OR";  ++i; continue; }
                if (i + 2 < end && ft[i] != ")" &&
                    toUpper(ft[i+1]) != "AND" && toUpper(ft[i+1]) != "OR" &&
                    ft[i+1] != ")") {
                    SubCond cond;
                    cond.col = ft[i];
                    cond.op  = ft[i+1];
                    cond.val = ft[i+2];
                    i += 3;
                    spec.conds.push_back(std::move(cond));
                } else { break; }
            }
        }

        // Schließendes ) überspringen
        while (i < end && ft[i] != ")") ++i;
        if (i < end && ft[i] == ")") ++i;
    }

    // IN-Liste oder Subquery aus tokenizeFull-Tokens lesen.
    // ft[i] sollte "(" sein.
    static void parseInListFromFull(const std::vector<std::string>& ft,
                                     size_t& i, size_t end,
                                     WhereCondition& cond,
                                     ParsedCommand& cmd) {
        if (i < end && ft[i] == "(") ++i;

        // Subquery: SELECT col FROM tbl [WHERE ...]
        if (i < end && toUpper(ft[i]) == "SELECT") {
            ++i;  // SELECT überspringen
            std::string subCol, subTable;
            if (i < end && ft[i] != ")" && ft[i] != ",") subCol   = ft[i++];
            if (i < end && toUpper(ft[i]) == "FROM") ++i;
            if (i < end && ft[i] != ")" && ft[i] != ",") subTable = ft[i++];

            // Subquery-WHERE-Bereich: bis zur schließenden ")"
            // (einfach: keine verschachtelten Subqueries)
            size_t subWhereStart = i;
            size_t subEnd = i;
            int depth = 1;
            while (subEnd < end) {
                if (ft[subEnd] == "(") ++depth;
                else if (ft[subEnd] == ")") { if (--depth == 0) break; }
                ++subEnd;
            }

            ParsedCommand subCmd;
            if (subWhereStart < subEnd && toUpper(ft[subWhereStart]) == "WHERE")
                parseWhereFromFull(ft, subWhereStart, subEnd, subCmd);

            i = subEnd;
            if (i < end && ft[i] == ")") ++i;

            ParsedCommand::SubquerySpec spec;
            spec.condIdx       = cmd.whereConds.size();
            spec.subTable      = subTable;
            spec.subCol        = subCol;
            spec.subWhere      = subCmd.whereConds;
            spec.subWhereLogic = subCmd.whereLogic;
            cmd.subqueries.push_back(std::move(spec));

        } else {
            // Werteliste: (v1 , v2 , v3)
            while (i < end && ft[i] != ")") {
                if (ft[i] != ",") cond.inList.push_back(ft[i]);
                ++i;
            }
            if (i < end && ft[i] == ")") ++i;
        }
    }

    // EXISTS-Subquery aus tokenizeFull-Tokens lesen.
    // Phase 37: Unterstützt mehrere WHERE-Bedingungen und Sub-Tabellen-Alias.
    static void parseExistsFromFull(const std::vector<std::string>& ft,
                                     size_t& i, size_t end,
                                     WhereCondition& cond) {
        if (i < end && ft[i] == "(") ++i;
        if (i < end && toUpper(ft[i]) == "SELECT") ++i;
        // SELECT-Ausdruck überspringen (typisch "1", "*" oder Spaltenname)
        while (i < end && ft[i] != ")" && toUpper(ft[i]) != "FROM") ++i;
        // FROM
        if (i < end && toUpper(ft[i]) == "FROM") ++i;
        // Tabellenname
        if (i < end && ft[i] != ")" && toUpper(ft[i]) != "WHERE")
            cond.existsSpec.subTable = ft[i++];
        // Optionaler Sub-Tabellen-Alias überspringen
        if (i < end && ft[i] != ")" && toUpper(ft[i]) != "WHERE")
            ++i;

        // WHERE: mehrere Bedingungen (Phase 37)
        if (i < end && toUpper(ft[i]) == "WHERE") {
            ++i;
            while (i < end && ft[i] != ")") {
                std::string u = toUpper(ft[i]);
                if (u == "AND") { cond.existsSpec.subWhereLogic = "AND"; ++i; continue; }
                if (u == "OR")  { cond.existsSpec.subWhereLogic = "OR";  ++i; continue; }
                if (i + 2 < end && ft[i] != ")" &&
                    toUpper(ft[i+1]) != "AND" && toUpper(ft[i+1]) != "OR" &&
                    ft[i+1] != ")") {
                    SubCond sc;
                    sc.col = ft[i];
                    sc.op  = ft[i+1];
                    sc.val = ft[i+2];
                    i += 3;
                    cond.existsSpec.subConds.push_back(std::move(sc));
                } else { break; }
            }
            // Legacy-Kompatibilität: erste Bedingung auch in alten Feldern
            if (!cond.existsSpec.subConds.empty()) {
                cond.existsSpec.condLeft  = cond.existsSpec.subConds[0].col;
                cond.existsSpec.condOp    = cond.existsSpec.subConds[0].op;
                cond.existsSpec.condRight = cond.existsSpec.subConds[0].val;
            }
        }
        // Schließende ")" überspringen
        while (i < end && ft[i] != ")") ++i;
        if (i < end && ft[i] == ")") ++i;
    }

    // Vollst��ndiges SELECT ohne JOIN und GROUP BY,
    // aber mit IN/NOT IN Unterstützung.
    // Verwendet tokenizeFull für korrekte Klammer-Behandlung.
    static void parseSelectFull(const std::string& raw, ParsedCommand& cmd) {
        cmd.type = CommandType::SELECT;
        auto ft = tokenizeFull(raw);
        const size_t N = ft.size();

        size_t fromPos  = N, wherePos = N;
        size_t orderPos = N, limitPos = N;
        // Phase 37: Paren-Tiefe verfolgen, damit FROM/WHERE in Subqueries ignoriert werden
        {
            int depth = 0;
            for (size_t i = 1; i < N; ++i) {  // i=1: SELECT überspringen
                if (ft[i] == "(") { ++depth; continue; }
                if (ft[i] == ")") { if (depth > 0) --depth; continue; }
                if (depth > 0) continue;  // innerhalb Subquery → ignorieren
                std::string u = toUpper(ft[i]);
                if (u == "FROM"  && fromPos  == N) fromPos  = i;
                if (u == "WHERE" && wherePos == N) wherePos = i;
                if (u == "ORDER" && orderPos == N) orderPos = i;
                if (u == "LIMIT" && limitPos == N) limitPos = i;
            }
        }

        if (fromPos == N || fromPos + 1 >= N) {
            // Phase 55: SELECT ohne FROM — z.B. SELECT NOW(), SELECT 1+1
            cmd.tableName = "";  // kein Tabellenname → Auswertung gegen leere Zeile
            // Spaltenbereich: alles nach SELECT bis Ende (skip DISTINCT)
            size_t noFromSelStart = 1;
            if (noFromSelStart < N && toUpper(ft[noFromSelStart]) == "DISTINCT") {
                cmd.isDistinct = true; ++noFromSelStart;
            }
            cmd.hasCaseItems = true;
            for (size_t i = noFromSelStart; i < N; ) {
                if (ft[i] == ",") { ++i; continue; }
                std::string u = toUpper(ft[i]);
                // Ist es eine Funktion mit ()?
                static const std::vector<std::string> DATE0ARGS =
                    {"NOW", "CURDATE", "CURTIME", "CURRENT_TIMESTAMP", "CURRENT_DATE", "CURRENT_TIME"};
                bool isDate0 = false;
                for (const auto& f : DATE0ARGS) if (u == f) { isDate0 = true; break; }
                if (isDate0) {
                    SelectItem item; item.isFuncExpr = true; item.funcName = u; ++i;
                    if (i < N && ft[i] == "(") { ++i; if (i < N && ft[i] == ")") ++i; }
                    if (i < N && toUpper(ft[i]) == "AS") { ++i; if (i < N && ft[i] != ",") { item.alias = ft[i]; ++i; } }
                    cmd.selectItems.push_back(item);
                    continue;
                }
                // Generic: collect tokens until next comma (top-level)
                std::string expr;
                int depth = 0;
                while (i < N) {
                    if (ft[i] == "(") { depth++; expr += ft[i] + " "; i++; continue; }
                    if (ft[i] == ")") { if (depth == 0) break; depth--; expr += ft[i] + " "; i++; continue; }
                    if (depth == 0 && ft[i] == ",") break;
                    expr += ft[i] + " ";
                    i++;
                }
                while (!expr.empty() && expr.back() == ' ') expr.pop_back();
                // Try to detect FUNC(...) pattern
                SelectItem item;
                {
                    // Tokenize the expression to find func name
                    auto exprToks = tokenizeFull(expr);
                    static const std::vector<std::string> ALLFUNCS =
                        {"UPPER","LOWER","LENGTH","CONCAT","SUBSTR","TRIM","REPLACE",
                         "ABS","ROUND","MOD","POWER","SQRT","CEIL","FLOOR",
                         "IFNULL","COALESCE","CAST",
                         "NOW","CURDATE","CURTIME","DATE","TIME",
                         "YEAR","MONTH","DAY","HOUR","MINUTE","SECOND",
                         "DATEDIFF","DATE_ADD","DATE_SUB","DATE_FORMAT",
                 // Phase 64: REGEXP-Funktionen
                 "REGEXP_REPLACE","REGEXP_EXTRACT"};
                    if (exprToks.size() >= 3 && exprToks[1] == "(") {
                        std::string fn = toUpper(exprToks[0]);
                        bool isKnown = false;
                        for (const auto& f : ALLFUNCS) if (fn == f) { isKnown = true; break; }
                        if (isKnown) {
                            item.isFuncExpr = true; item.funcName = fn;
                            // collect args between outer parens
                            int d = 0; std::string curArg;
                            for (size_t j = 2; j < exprToks.size(); ++j) {
                                if (exprToks[j] == "(") { d++; curArg += exprToks[j] + " "; continue; }
                                if (exprToks[j] == ")") {
                                    if (d == 0) break;
                                    d--; curArg += exprToks[j] + " "; continue;
                                }
                                if (d == 0 && exprToks[j] == ",") {
                                    while (!curArg.empty() && curArg.back() == ' ') curArg.pop_back();
                                    if (!curArg.empty()) item.funcArgs.push_back(curArg);
                                    curArg = ""; continue;
                                }
                                curArg += exprToks[j] + " ";
                            }
                            while (!curArg.empty() && curArg.back() == ' ') curArg.pop_back();
                            if (!curArg.empty()) item.funcArgs.push_back(curArg);
                        }
                    }
                    if (!item.isFuncExpr) {
                        // Check AS alias
                        item.colName = expr;
                        // Look for " AS " pattern
                        std::string upper = expr;
                        for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                        auto asPos = upper.rfind(" AS ");
                        if (asPos != std::string::npos) {
                            item.alias   = expr.substr(asPos + 4);
                            item.colName = expr.substr(0, asPos);
                        }
                    } else {
                        // Parse AS alias after closing paren
                        if (i < N && toUpper(ft[i]) == "AS") {
                            ++i;
                            if (i < N && ft[i] != ",") { item.alias = ft[i]; ++i; }
                        }
                    }
                }
                cmd.selectItems.push_back(item);
            }
            return;
        }
        cmd.tableName = ft[fromPos + 1];

        // Phase 37: Alias nach Tabellenname erkennen (FROM tbl alias)
        if (fromPos + 2 < N) {
            std::string next = toUpper(ft[fromPos + 2]);
            if (next != "WHERE" && next != "ORDER" && next != "LIMIT" &&
                next != "GROUP" && next != "HAVING" && next != "INNER" &&
                next != "LEFT" && next != "RIGHT" && next != "FULL" &&
                next != "JOIN" && next != "ON" && next != "UNION" &&
                next != "INTERSECT" && next != "EXCEPT" &&
                ft[fromPos + 2] != "," && ft[fromPos + 2] != "(" &&
                ft[fromPos + 2] != ")") {
                cmd.tableAlias = ft[fromPos + 2];
            }
        }

        // SELECT-Spalten
        size_t selStart = 1;
        if (selStart < fromPos && toUpper(ft[selStart]) == "DISTINCT") {
            cmd.isDistinct = true; ++selStart;
        }
        // Phase 31/32/64: CASE oder String-Funktion im Spaltenbereich?
        static const std::vector<std::string> SFUNCS32 =
            {"UPPER", "LOWER", "LENGTH", "CONCAT", "SUBSTR", "TRIM", "REPLACE",
                 "ABS", "ROUND", "MOD", "POWER", "SQRT", "CEIL", "FLOOR",
                 "IFNULL", "COALESCE", "CAST", "MATCH",
                 // Phase 55: DATE/TIME-Funktionen
                 "NOW", "CURDATE", "CURTIME", "DATE", "TIME",
                 "YEAR", "MONTH", "DAY", "HOUR", "MINUTE", "SECOND",
                 "DATEDIFF", "DATE_ADD", "DATE_SUB", "DATE_FORMAT",
                 "CURRENT_TIMESTAMP", "CURRENT_DATE", "CURRENT_TIME",
                 // Phase 56: JSON-Funktionen
                 "JSON_EXTRACT", "JSON_SET", "JSON_KEYS", "JSON_LENGTH",
                 "JSON_CONTAINS", "JSON_TYPE", "JSON_VALID",
                 // Phase 64: REGEXP-Funktionen
                 "REGEXP_REPLACE", "REGEXP_EXTRACT",
                 // Phase 70: Spatial-Funktionen
                 "ST_DISTANCE", "ST_X", "ST_Y", "ST_WITHIN", "ST_ASTEXT"};
        bool hasCase = false, hasFunc = false;
        for (size_t i = selStart; i < fromPos && !(hasCase && hasFunc); ++i) {
            std::string u = toUpper(ft[i]);
            if (u == "CASE") { hasCase = true; continue; }
            for (const auto& f : SFUNCS32) if (u == f) { hasFunc = true; break; }
        }

        // Phase 37: Scalar Subquery in SELECT-Liste?
        bool hasScalarSub37 = false;
        for (size_t i = selStart; i < fromPos; ++i) {
            if (ft[i] == "(" && i + 1 < fromPos && toUpper(ft[i+1]) == "SELECT") {
                hasScalarSub37 = true; break;
            }
        }

        // Phase 42: Window function (OVER keyword) in SELECT-Liste?
        bool hasWindowFunc = false;
        for (size_t i = selStart; i < fromPos; ++i) {
            if (toUpper(ft[i]) == "OVER") { hasWindowFunc = true; break; }
        }

        if (hasCase || hasFunc || hasScalarSub37 || hasWindowFunc) {
            parseCaseSelectItems(ft, selStart, fromPos, cmd);
        } else {
            std::string colList;
            for (size_t i = selStart; i < fromPos; ++i) {
                if (ft[i] == "," || ft[i] == "(" || ft[i] == ")") continue;
                if (!colList.empty()) colList += ",";
                colList += ft[i];
            }
            for (const auto& c : splitTrim(colList, ','))
                if (c != "*" && !c.empty()) cmd.selectColumns.push_back(c);
        }

        // WHERE (mit IN-Unterstützung)
        if (wherePos != N) {
            size_t whereEnd = std::min({orderPos, limitPos, N});
            parseWhereFromFull(ft, wherePos, whereEnd, cmd);
        }

        // ORDER BY (Phase 38: multi-column)
        if (orderPos != N && orderPos + 2 < N &&
            toUpper(ft[orderPos + 1]) == "BY") {
            parseOrderByCols(ft, orderPos + 2, limitPos < N ? limitPos : N, cmd.orderByCols);
        }

        // LIMIT [OFFSET] (Phase 38)
        if (limitPos != N && limitPos + 1 < N) {
            try { cmd.limit = std::stoi(ft[limitPos + 1]); } catch (...) {}
            if (limitPos + 3 < N && toUpper(ft[limitPos + 2]) == "OFFSET") {
                try { cmd.limitOffset = std::stoi(ft[limitPos + 3]); } catch (...) {}
            }
        }
    }

    // ── Phase 31: SELECT-Liste mit CASE WHEN parsen ──────────────
    // Verarbeitet normale Spalten UND CASE-Ausdrücke in der SELECT-Liste.
    // Ergebnis geht in cmd.selectItems; cmd.hasCaseItems wird gesetzt.
    static void parseCaseSelectItems(const std::vector<std::string>& ft,
                                     size_t start, size_t end,
                                     ParsedCommand& cmd) {
        size_t i = start;
        while (i < end) {
            // Phase 37: (SELECT ...) → Skalare Subquery
            if (ft[i] == "(") {
                if (i + 1 < end && toUpper(ft[i+1]) == "SELECT") {
                    ++i;  // skip (
                    SelectItem item;
                    item.isScalarSubquery = true;
                    parseScalarSubFromFull(ft, i, end, item.scalarSub);
                    // optionales AS alias
                    if (i < end && toUpper(ft[i]) == "AS") {
                        ++i;
                        if (i < end) { item.alias = ft[i]; ++i; }
                    }
                    cmd.selectItems.push_back(std::move(item));
                    cmd.hasCaseItems = true;
                    continue;
                }
                ++i; continue;  // gewöhnliche ( überspringen
            }
            if (ft[i] == "," || ft[i] == ")") { ++i; continue; }

            std::string u = toUpper(ft[i]);

            if (u == "CASE") {
                // ── CASE WHEN col op val THEN res ... [ELSE res] END [AS alias]
                SelectItem item;
                item.isCaseExpr = true;
                ++i;  // skip CASE

                while (i < end) {
                    std::string wu = toUpper(ft[i]);

                    if (wu == "WHEN") {
                        ++i;  // skip WHEN
                        // col  op  val  THEN  result
                        if (i + 4 < end && toUpper(ft[i + 3]) == "THEN") {
                            SelectItem::WhenClause wh;
                            wh.col    = ft[i];
                            wh.op     = ft[i + 1];
                            wh.val    = ft[i + 2];
                            wh.result = ft[i + 4];
                            i += 5;
                            item.caseWhen.push_back(wh);
                        } else if (i + 2 < end) {
                            // Kein THEN gefunden — trotzdem partiell lesen
                            SelectItem::WhenClause wh;
                            wh.col = ft[i]; wh.op = ft[i+1]; wh.val = ft[i+2];
                            i += 3;
                            item.caseWhen.push_back(wh);
                        }

                    } else if (wu == "ELSE") {
                        ++i;  // skip ELSE
                        if (i < end && toUpper(ft[i]) != "END")
                        { item.caseElse = ft[i]; ++i; }

                    } else if (wu == "END") {
                        ++i;  // skip END
                        // optionales AS alias
                        if (i < end && toUpper(ft[i]) == "AS") {
                            ++i;
                            if (i < end && ft[i] != ",") { item.alias = ft[i]; ++i; }
                        }
                        break;
                    } else {
                        ++i;
                    }
                }
                cmd.selectItems.push_back(item);

            } else if (u == "*") {
                // SELECT * bleibt als Wildcard erhalten
                SelectItem item; item.colName = "*";
                cmd.selectItems.push_back(item);
                ++i;

            } else {
                // Phase 32/40: String-Funktion oder CAST? FUNC ( args... ) [AS alias]
                static const std::vector<std::string> SFUNCS32 =
                    {"UPPER", "LOWER", "LENGTH", "CONCAT", "SUBSTR", "TRIM", "REPLACE",
                 "ABS", "ROUND", "MOD", "POWER", "SQRT", "CEIL", "FLOOR",
                 "IFNULL", "COALESCE", "CAST", "MATCH",
                 // Phase 55: DATE/TIME-Funktionen
                 "NOW", "CURDATE", "CURTIME", "DATE", "TIME",
                 "YEAR", "MONTH", "DAY", "HOUR", "MINUTE", "SECOND",
                 "DATEDIFF", "DATE_ADD", "DATE_SUB", "DATE_FORMAT",
                 "CURRENT_TIMESTAMP", "CURRENT_DATE", "CURRENT_TIME",
                 // Phase 56: JSON-Funktionen
                 "JSON_EXTRACT", "JSON_SET", "JSON_KEYS", "JSON_LENGTH",
                 "JSON_CONTAINS", "JSON_TYPE", "JSON_VALID",
                 // Phase 64: REGEXP-Funktionen
                 "REGEXP_REPLACE", "REGEXP_EXTRACT",
                 // Phase 70: Spatial-Funktionen
                 "ST_DISTANCE", "ST_X", "ST_Y", "ST_WITHIN", "ST_ASTEXT"};
                bool isStrFunc = false;
                for (const auto& f : SFUNCS32) if (u == f) { isStrFunc = true; break; }

                // Phase 42: Window functions
                static const std::vector<std::string> WFUNCS42 =
                    {"ROW_NUMBER", "RANK", "DENSE_RANK", "SUM", "AVG", "COUNT", "MIN", "MAX"};
                bool isWinFunc = false;
                for (const auto& f : WFUNCS42) if (u == f) { isWinFunc = true; break; }

                // Peek ahead: if this function is followed by "(" and then "OVER" after closing ")",
                // treat as window function.
                // Check: is this a window function call? Look for OVER after the argument list.
                bool treatAsWindow = false;
                if (isWinFunc && i + 1 < end && ft[i + 1] == "(") {
                    // Scan ahead to find closing ")" and check if next token is "OVER"
                    int scanDepth = 0;
                    size_t scanI = i + 1;
                    while (scanI < end) {
                        if (ft[scanI] == "(") ++scanDepth;
                        else if (ft[scanI] == ")") {
                            --scanDepth;
                            if (scanDepth == 0) { ++scanI; break; }
                        }
                        ++scanI;
                    }
                    if (scanI < end && toUpper(ft[scanI]) == "OVER")
                        treatAsWindow = true;
                }
                // Also: SUM/AVG/COUNT/MIN/MAX can appear without OVER (in GROUP BY context)
                // so only treat as window if OVER follows.

                if (treatAsWindow) {
                    // Parse window function: FUNC ( [arg] ) OVER ( [PARTITION BY col] [ORDER BY col [DESC]] )
                    SelectItem item;
                    item.isWindowFunc = true;
                    item.windowFunc   = u;  // already uppercase
                    ++i;  // skip function name

                    // Parse function arguments (between parens)
                    if (i < end && ft[i] == "(") {
                        ++i;  // skip "("
                        // Collect args until matching ")"
                        std::string argStr;
                        int depth = 0;
                        while (i < end) {
                            if (ft[i] == "(") { depth++; argStr += ft[i] + " "; ++i; continue; }
                            if (ft[i] == ")") {
                                if (depth == 0) { ++i; break; }
                                depth--; argStr += ft[i] + " "; ++i; continue;
                            }
                            if (depth == 0 && ft[i] == ",") { ++i; continue; }
                            argStr += ft[i];
                            ++i;
                        }
                        // Trim
                        while (!argStr.empty() && argStr.back() == ' ') argStr.pop_back();
                        item.windowFuncArg = argStr;  // e.g. "gehalt" or "*" or ""
                    }

                    // Skip OVER
                    if (i < end && toUpper(ft[i]) == "OVER") ++i;

                    // Parse OVER ( ... )
                    if (i < end && ft[i] == "(") {
                        ++i;  // skip "("
                        // Collect inner tokens until matching ")"
                        std::vector<std::string> overToks;
                        int depth = 0;
                        while (i < end) {
                            if (ft[i] == "(") { depth++; overToks.push_back(ft[i]); ++i; continue; }
                            if (ft[i] == ")") {
                                if (depth == 0) { ++i; break; }
                                depth--; overToks.push_back(ft[i]); ++i; continue;
                            }
                            overToks.push_back(ft[i]);
                            ++i;
                        }
                        // Parse OVER clause tokens
                        size_t oi = 0;
                        while (oi < overToks.size()) {
                            std::string ou = toUpper(overToks[oi]);
                            if (ou == "PARTITION" && oi + 2 < overToks.size() &&
                                toUpper(overToks[oi + 1]) == "BY") {
                                oi += 2;
                                if (oi < overToks.size()) {
                                    item.windowPartitionBy = overToks[oi]; ++oi;
                                }
                            } else if (ou == "ORDER" && oi + 2 < overToks.size() &&
                                       toUpper(overToks[oi + 1]) == "BY") {
                                oi += 2;
                                if (oi < overToks.size()) {
                                    item.windowOrderBy = overToks[oi]; ++oi;
                                    if (oi < overToks.size() && toUpper(overToks[oi]) == "DESC") {
                                        item.windowOrderDesc = true; ++oi;
                                    } else if (oi < overToks.size() && toUpper(overToks[oi]) == "ASC") {
                                        item.windowOrderDesc = false; ++oi;
                                    }
                                }
                            } else {
                                ++oi;
                            }
                        }
                    }

                    // Optional AS alias
                    if (i < end && toUpper(ft[i]) == "AS") {
                        ++i;
                        if (i < end && ft[i] != ",") { item.alias = ft[i]; ++i; }
                    }
                    cmd.selectItems.push_back(item);

                } else if (isStrFunc && (i + 1 >= end || ft[i + 1] != "(") &&
                           (u == "NOW" || u == "CURDATE" || u == "CURTIME" ||
                            u == "CURRENT_TIMESTAMP" || u == "CURRENT_DATE" || u == "CURRENT_TIME")) {
                    // Phase 55: Zero-arg date functions used without ()
                    SelectItem item;
                    item.isFuncExpr = true;
                    item.funcName   = u;
                    ++i;
                    // skip "()" if present
                    if (i < end && ft[i] == "(") {
                        ++i;
                        if (i < end && ft[i] == ")") ++i;
                    }
                    if (i < end && toUpper(ft[i]) == "AS") {
                        ++i;
                        if (i < end && ft[i] != ",") { item.alias = ft[i]; ++i; }
                    }
                    cmd.selectItems.push_back(item);
                } else if (isStrFunc && i + 1 < end && ft[i + 1] == "(") {
                    SelectItem item;
                    item.isFuncExpr = true;
                    item.funcName   = u;
                    i += 2;  // Funktionsname + "(" überspringen
                    // Phase 40: Depth-aware arg collection
                    // For CAST: split on AS at depth 0
                    // For others: split on , at depth 0
                    {
                        bool isCast = (u == "CAST");
                        int depth = 0;
                        std::string currentArg;
                        while (i < end) {
                            if (ft[i] == "(") {
                                depth++;
                                currentArg += ft[i] + " ";
                                i++; continue;
                            }
                            if (ft[i] == ")") {
                                if (depth == 0) break;  // end of our function
                                depth--;
                                currentArg += ft[i] + " ";
                                i++; continue;
                            }
                            if (depth == 0 && ft[i] == ",") {
                                std::string a = currentArg;
                                while (!a.empty() && a.back() == ' ') a.pop_back();
                                if (!a.empty()) item.funcArgs.push_back(a);
                                currentArg = "";
                                i++; continue;
                            }
                            if (depth == 0 && isCast && toUpper(ft[i]) == "AS") {
                                std::string a = currentArg;
                                while (!a.empty() && a.back() == ' ') a.pop_back();
                                if (!a.empty()) item.funcArgs.push_back(a);
                                currentArg = "";
                                i++; continue;
                            }
                            currentArg += ft[i] + " ";
                            i++;
                        }
                        // push final arg
                        {
                            std::string a = currentArg;
                            while (!a.empty() && a.back() == ' ') a.pop_back();
                            if (!a.empty()) item.funcArgs.push_back(a);
                        }
                    }
                    if (i < end && ft[i] == ")") ++i;

                    // Phase 49: MATCH(...) AGAINST('query') — treat specially
                    if (u == "MATCH") {
                        item.isMatchAgainst = true;
                        item.isFuncExpr = false;
                        item.matchCols = item.funcArgs;
                        item.funcArgs.clear();
                        // Parse AGAINST ( 'query' )
                        if (i < end && toUpper(ft[i]) == "AGAINST") {
                            ++i; // skip AGAINST
                            if (i < end && ft[i] == "(") ++i; // skip (
                            while (i < end && ft[i] != ")") {
                                std::string qt = ft[i];
                                if (qt.size() >= 2 && qt.front() == '\'' && qt.back() == '\'')
                                    qt = qt.substr(1, qt.size() - 2);
                                if (!item.againstQuery.empty()) item.againstQuery += " ";
                                item.againstQuery += qt;
                                ++i;
                            }
                            if (i < end && ft[i] == ")") ++i; // skip )
                        }
                    }

                    // optionales AS alias
                    if (i < end && toUpper(ft[i]) == "AS") {
                        ++i;
                        if (i < end && ft[i] != ",") { item.alias = ft[i]; ++i; }
                    }
                    cmd.selectItems.push_back(item);
                } else {
                    // Normale Spalte, optional mit AS alias
                    SelectItem item;
                    item.colName = ft[i]; ++i;
                    if (i < end && toUpper(ft[i]) == "AS") {
                        ++i;
                        if (i < end && ft[i] != ",") { item.alias = ft[i]; ++i; }
                    }
                    cmd.selectItems.push_back(item);
                }
            }
        }
        cmd.hasCaseItems = true;
    }

    // ── Phase 30: Set-Operation Parser ───────────────────────────
    // Syntax: SELECT ... UNION [ALL] SELECT ...
    //         SELECT ... INTERSECT SELECT ...
    //         SELECT ... EXCEPT SELECT ...
    // Sucht das erste UNION/INTERSECT/EXCEPT auf Top-Level (nicht in
    // Klammern) und spaltet den Input in linke + rechte Seite.
    static void parseSetOp(const std::string& raw, ParsedCommand& cmd) {
        cmd.type    = CommandType::SELECT;
        cmd.isSetOp = true;

        // Tokenize + Set-Op-Position finden
        auto ft = tokenizeFull(raw);
        const size_t N = ft.size();
        size_t opPos = N;
        std::string opName;

        int depth = 0;
        for (size_t i = 0; i < N; ++i) {
            if (ft[i] == "(") { ++depth; continue; }
            if (ft[i] == ")") { --depth; continue; }
            if (depth > 0) continue;

            std::string u = toUpper(ft[i]);
            if ((u == "UNION" || u == "INTERSECT" || u == "EXCEPT") && opPos == N) {
                opName = u;
                opPos  = i;
                // UNION ALL? — nächstes Token prüfen
                if (u == "UNION" && i + 1 < N && toUpper(ft[i + 1]) == "ALL") {
                    opName = "UNION ALL";
                    ++i;  // "ALL" überspringen
                    opPos = i;  // opPos zeigt jetzt auf "ALL" — egal, splitten nach Rohtext
                }
                break;
            }
        }

        if (opPos == N) { cmd.type = CommandType::UNKNOWN; return; }

        // Rohtext splitten: links alles vor dem Keyword, rechts alles danach
        // Suche im Originaltext nach dem Operator (case-insensitive)
        std::string rawUp = raw;
        for (char& c : rawUp)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        size_t splitPos = std::string::npos;
        if (opName == "UNION ALL") {
            // Suche "UNION ALL" als Substring
            splitPos = rawUp.find("UNION ALL");
            if (splitPos != std::string::npos)
                cmd.rightSql = trim(raw.substr(splitPos + 9));  // len("UNION ALL")=9
        } else {
            splitPos = rawUp.find(opName);
            if (splitPos != std::string::npos)
                cmd.rightSql = trim(raw.substr(splitPos + opName.size()));
        }

        if (splitPos == std::string::npos) { cmd.type = CommandType::UNKNOWN; return; }

        cmd.setOp = opName;

        // Linke Seite als normales SELECT parsen
        std::string leftSql = trim(raw.substr(0, splitPos));
        // Parsed the left SELECT inline (recycled parseSelectFull)
        parseSelectFull(leftSql, cmd);   // befüllt tableName, whereConds, selectColumns usw.
        cmd.isSetOp  = true;             // parseSelectFull setzt isSetOp nicht — wiederherstellen
        cmd.setOp    = opName;
        cmd.rightSql = (splitPos != std::string::npos)
            ? (opName == "UNION ALL"
               ? trim(raw.substr(rawUp.find("UNION ALL") + 9))
               : trim(raw.substr(rawUp.find(opName) + opName.size())))
            : "";
    }

    // ── Phase 12: JOIN-Parser (INNER + LEFT, mehrere JOINs) ──────

    // Syntax:
    //   SELECT cols FROM t1 [LEFT|INNER] JOIN t2 ON t1.c = t2.c
    //                       [LEFT|INNER] JOIN t3 ON t2.c = t3.c ...
    //   [WHERE ...] [ORDER BY ...] [LIMIT N]
    static void parseJoinQuery(const std::string& raw, ParsedCommand& cmd) {
        cmd.type   = CommandType::SELECT;
        cmd.isJoin = true;

        auto ft = tokenizeFull(raw);
        const size_t N = ft.size();

        // Globale Stop-Positionen
        size_t fromPos  = N, wherePos = N;
        size_t orderPos = N, limitPos = N;

        for (size_t i = 0; i < N; ++i) {
            std::string u = toUpper(ft[i]);
            if      (u == "FROM"  && fromPos  == N) fromPos  = i;
            else if (u == "WHERE" && wherePos == N) wherePos = i;
            else if (u == "ORDER" && orderPos == N) orderPos = i;
            else if (u == "LIMIT" && limitPos == N) limitPos = i;
        }

        if (fromPos == N || fromPos + 1 >= N) {
            cmd.type = CommandType::UNKNOWN; return;
        }

        cmd.tableName = ft[fromPos + 1];

        // SELECT-Spalten (zwischen SELECT/DISTINCT und FROM)
        size_t selStart = 1;
        if (selStart < fromPos && toUpper(ft[selStart]) == "DISTINCT") {
            cmd.isDistinct = true; ++selStart;
        }
        std::string colList;
        for (size_t i = selStart; i < fromPos; ++i) {
            if (ft[i] == ",") continue;
            if (!colList.empty()) colList += ",";
            colList += ft[i];
        }
        for (const auto& c : splitTrim(colList, ','))
            if (c != "*" && !c.empty())
                cmd.selectColumns.push_back(c);

        // JOIN-Klauseln scannen (zwischen Basistabelle und WHERE/ORDER/LIMIT)
        size_t scanEnd = std::min({wherePos, orderPos, limitPos, N});
        size_t i = fromPos + 2;
        std::string curType = "INNER";  // Standard-JOIN-Typ

        while (i < scanEnd) {
            std::string u = toUpper(ft[i]);

            if (u == "LEFT")  { curType = "LEFT";  ++i; continue; }
            if (u == "INNER") { curType = "INNER"; ++i; continue; }
            if (u == "RIGHT") { curType = "RIGHT"; ++i; continue; }
            if (u == "FULL")  { curType = "FULL";  ++i; continue; }
            if (u == "OUTER") {                    ++i; continue; }  // FULL OUTER JOIN

            if (u == "JOIN") {
                // Erwartet: table ON left = right
                if (i + 3 >= scanEnd) { cmd.type = CommandType::UNKNOWN; return; }

                JoinClause jc;
                jc.joinType = curType;
                jc.table    = ft[i + 1];

                if (toUpper(ft[i + 2]) != "ON") {
                    cmd.type = CommandType::UNKNOWN; return;
                }

                // ON-Bedingung: 3 Tokens "left = right" oder 1 Token "left=right"
                if (i + 5 < scanEnd && ft[i + 4] == "=") {
                    jc.onLeft  = ft[i + 3];
                    jc.onRight = ft[i + 5];
                    i += 6;
                } else if (i + 3 < scanEnd) {
                    const std::string& tok = ft[i + 3];
                    auto eq = tok.find('=');
                    if (eq == std::string::npos) {
                        cmd.type = CommandType::UNKNOWN; return;
                    }
                    jc.onLeft  = tok.substr(0, eq);
                    jc.onRight = tok.substr(eq + 1);
                    i += 4;
                } else {
                    cmd.type = CommandType::UNKNOWN; return;
                }

                cmd.joinClauses.push_back(std::move(jc));
                curType = "INNER";  // zurücksetzen
                continue;
            }

            ++i;
        }

        if (cmd.joinClauses.empty()) { cmd.type = CommandType::UNKNOWN; return; }

        // WHERE
        if (wherePos != N) {
            size_t whereEnd = std::min({orderPos, limitPos, N});
            std::vector<std::string> wt;
            for (size_t k = wherePos; k < whereEnd; ++k) {
                const std::string& t = ft[k];
                if (t != "(" && t != ")" && t != ",") wt.push_back(t);
            }
            if (!wt.empty()) parseWhere(wt, 0, cmd);
        }

        // ORDER BY (Phase 38: multi-column)
        if (orderPos != N && orderPos + 2 < N &&
            toUpper(ft[orderPos + 1]) == "BY") {
            parseOrderByCols(ft, orderPos + 2, limitPos < N ? limitPos : N, cmd.orderByCols);
        }

        // LIMIT [OFFSET] (Phase 38)
        if (limitPos != N && limitPos + 1 < N) {
            try { cmd.limit = std::stoi(ft[limitPos + 1]); }
            catch (...) {}
            if (limitPos + 3 < N && toUpper(ft[limitPos + 2]) == "OFFSET") {
                try { cmd.limitOffset = std::stoi(ft[limitPos + 3]); } catch (...) {}
            }
        }
    }

    // ── Phase 10: GROUP BY-Parser ─────────────────────────────

    // tokenizeFull: wie tokenize, aber (, ), , werden als eigene Tokens ausgegeben.
    // Phase 32: 'literal'-Strings (Single-Quotes) bleiben als ein Token erhalten.
    static std::vector<std::string> tokenizeFull(const std::string& s) {
        std::vector<std::string> tokens;
        std::string cur;
        bool inQuote = false;
        for (char c : s) {
            if (inQuote) {
                cur += c;
                if (c == '\'') { inQuote = false; tokens.push_back(cur); cur.clear(); }
            } else if (c == '\'') {
                if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
                cur += c;
                inQuote = true;
            } else if (c == ' ' || c == '\t' || c == '\r') {
                if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
            } else if (c == '(' || c == ')' || c == ',') {
                if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
                tokens.push_back(std::string(1, c));
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) tokens.push_back(cur);
        return tokens;
    }

    // Vollständiges SELECT ... GROUP BY ... HAVING ... parsen
    static void parseGroupByQuery(const std::string& raw, ParsedCommand& cmd) {
        cmd.type      = CommandType::SELECT;
        cmd.isGroupBy = true;

        auto ft = tokenizeFull(raw);
        const size_t N = ft.size();

        // Schlüsselpositionen ermitteln (erste Treffer)
        size_t fromPos    = N, wherePos = N, groupPos = N;
        size_t byPos      = N, havingPos = N;
        size_t orderPos   = N, limitPos  = N;

        for (size_t i = 0; i < N; ++i) {
            std::string u = toUpper(ft[i]);
            if      (u == "FROM"   && fromPos  == N) fromPos  = i;
            else if (u == "WHERE"  && wherePos == N) wherePos = i;
            else if (u == "GROUP"  && groupPos == N) groupPos = i;
            else if (u == "BY"     && i > 0 &&
                     toUpper(ft[i - 1]) == "GROUP" && byPos == N) byPos = i;
            else if (u == "HAVING" && havingPos == N) havingPos = i;
            else if (u == "ORDER"  && orderPos == N) orderPos  = i;
            else if (u == "LIMIT"  && limitPos == N) limitPos  = i;
        }

        if (fromPos == N || fromPos + 1 >= N) {
            cmd.type = CommandType::UNKNOWN; return;
        }

        // Tabellenname
        cmd.tableName = ft[fromPos + 1];

        // SELECT-Liste (zwischen SELECT/DISTINCT und FROM)
        size_t selStart = 1;
        if (selStart < fromPos && toUpper(ft[selStart]) == "DISTINCT") {
            cmd.isDistinct = true; ++selStart;
        }
        parseSelectItems(ft, selStart, fromPos, cmd);

        // WHERE (zwischen FROM+2 und GROUP/ORDER/LIMIT/HAVING)
        if (wherePos != N) {
            size_t whereEnd = std::min({groupPos, orderPos, limitPos, havingPos, N});
            // Parens und Kommas herausfiltern (für einfaches WHERE)
            std::vector<std::string> wt;
            for (size_t i = wherePos; i < whereEnd; ++i) {
                const std::string& t = ft[i];
                if (t != "(" && t != ")" && t != ",")
                    wt.push_back(t);
            }
            if (!wt.empty()) parseWhere(wt, 0, cmd);
        }

        // GROUP BY-Spalten (nach BY bis HAVING/ORDER/LIMIT)
        if (groupPos != N && byPos != N) {
            size_t gcEnd = std::min({havingPos, orderPos, limitPos, N});
            for (size_t i = byPos + 1; i < gcEnd; ++i) {
                const std::string& t = ft[i];
                if (t != "," && t != "(" && t != ")")
                    cmd.groupByCols.push_back(t);
            }
        }

        // HAVING
        if (havingPos != N) {
            size_t hEnd = std::min({orderPos, limitPos, N});
            parseHavingFromFull(ft, havingPos + 1, hEnd, cmd);
        }

        // ORDER BY (Phase 38: multi-column; supports aggregate column names like COUNT(*))
        if (orderPos != N && orderPos + 2 < N &&
            toUpper(ft[orderPos + 1]) == "BY") {
            size_t obStart = orderPos + 2;
            size_t obEnd   = std::min(limitPos, N);
            // Rebuild obStart..obEnd into logical "col [ASC|DESC]" tokens,
            // collapsing FUNC ( col ) into a single "FUNC(col)" token.
            std::vector<std::string> obTokens;
            for (size_t i = obStart; i < obEnd; ) {
                std::string u = toUpper(ft[i]);
                static const std::vector<std::string> AGGF2 =
                    {"COUNT", "MIN", "MAX", "AVG", "SUM"};
                bool isAgg2 = false;
                for (const auto& f : AGGF2) if (u == f) { isAgg2 = true; break; }
                if (isAgg2 && i + 3 < obEnd && ft[i + 1] == "(") {
                    obTokens.push_back(u + "(" + ft[i + 2] + ")");
                    i += 4;  // skip FUNC ( col )
                } else if (ft[i] == "," || ft[i] == "(" || ft[i] == ")") {
                    obTokens.push_back(ft[i]); ++i;
                } else {
                    obTokens.push_back(ft[i]); ++i;
                }
            }
            parseOrderByCols(obTokens, 0, obTokens.size(), cmd.orderByCols);
        }

        // LIMIT [OFFSET] (Phase 38)
        if (limitPos != N && limitPos + 1 < N) {
            try { cmd.limit = std::stoi(ft[limitPos + 1]); }
            catch (...) { cmd.limit = -1; }
            if (limitPos + 3 < N && toUpper(ft[limitPos + 2]) == "OFFSET") {
                try { cmd.limitOffset = std::stoi(ft[limitPos + 3]); } catch (...) {}
            }
        }
    }

    // SELECT-Einträge aus Full-Token-Liste parsen
    // Erkennt: Aggregate (FUNC(col)), normale Spalten, *
    static void parseSelectItems(const std::vector<std::string>& ft,
                                  size_t start, size_t end, ParsedCommand& cmd) {
        static const std::vector<std::string> AGGF =
            {"COUNT", "MIN", "MAX", "AVG", "SUM"};

        size_t i = start;
        while (i < end) {
            const std::string& t = ft[i];
            if (t == ",") { ++i; continue; }

            std::string u = toUpper(t);
            bool isAgg = false;
            for (const auto& f : AGGF) if (u == f) { isAgg = true; break; }

            // Aggregatfunktion: FUNC ( col )
            if (isAgg && i + 3 < end && ft[i + 1] == "(") {
                SelectItem item;
                item.isAgg   = true;
                item.aggFunc = u;
                item.aggCol  = ft[i + 2];   // z. B. "*" oder "level"
                // ft[i+3] sollte ")" sein
                cmd.selectItems.push_back(std::move(item));
                i += 4;
            } else {
                // Normale Spalte (oder *)
                SelectItem item;
                item.isAgg   = false;
                item.colName = t;
                cmd.selectItems.push_back(std::move(item));
                ++i;
            }
        }
    }

    // HAVING-Bedingungen aus Full-Token-Liste parsen
    // Syntax: FUNC(col) op val [AND|OR FUNC(col) op val ...]
    static void parseHavingFromFull(const std::vector<std::string>& ft,
                                     size_t start, size_t end, ParsedCommand& cmd) {
        static const std::vector<std::string> AGGF =
            {"COUNT", "MIN", "MAX", "AVG", "SUM"};
        cmd.havingLogic = "AND";

        size_t i = start;
        while (i < end) {
            std::string u = toUpper(ft[i]);

            if (u == "AND") { cmd.havingLogic = "AND"; ++i; continue; }
            if (u == "OR")  { cmd.havingLogic = "OR";  ++i; continue; }

            bool isAgg = false;
            for (const auto& f : AGGF) if (u == f) { isAgg = true; break; }

            // Erwartet: FUNC ( col ) op val  → 6 Tokens
            if (isAgg && i + 5 < end && ft[i + 1] == "(") {
                HavingCondition hc;
                hc.aggFunc = u;
                hc.aggCol  = ft[i + 2];   // ft[i+3] = ")"
                hc.op      = ft[i + 4];
                hc.val     = ft[i + 5];
                cmd.havingConds.push_back(std::move(hc));
                i += 6;
            } else {
                ++i;  // unbekanntes Token überspringen
            }
        }
    }

    // ── Phase 20: FOREIGN KEY Parser-Helpers ─────────────────

    // Extrahiert den Inhalt aus "(content)" → "content"
    static std::string extractParen(const std::string& s) {
        auto p1 = s.find('(');
        auto p2 = s.find(')');
        if (p1 == std::string::npos) return "";
        size_t end = (p2 != std::string::npos) ? p2 : s.size();
        return trim(s.substr(p1 + 1, end - p1 - 1));
    }

    // Zerlegt "refTable(refCol)" → {"refTable", "refCol"}
    static std::pair<std::string, std::string> splitRefPart(const std::string& s) {
        auto p = s.find('(');
        if (p == std::string::npos) return {s, ""};
        std::string tbl = trim(s.substr(0, p));
        std::string col = extractParen(s);
        return {tbl, col};
    }

    // ── Basis-Hilfsmethoden ───────────────────────────────────

    static std::vector<std::string> tokenize(const std::string& s) {
        std::vector<std::string> tokens;
        std::string cur;
        for (char c : s) {
            if (c == ' ' || c == '\t' || c == '\r') {
                if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
            } else { cur += c; }
        }
        if (!cur.empty()) tokens.push_back(cur);
        return tokens;
    }

    // Phase 56: Tiefenkenner Wert-Splitter
    // Wie splitTrim, aber ignoriert Kommas innerhalb von {}, [], '' und ""
    static std::vector<std::string> splitValues(const std::string& s) {
        std::vector<std::string> result;
        std::string cur;
        int depth = 0;  // Tiefe von (), {}, []
        bool inStr = false;
        char strChar = 0;
        for (size_t i = 0; i < s.size(); ++i) {
            char c = s[i];
            if (inStr) {
                cur += c;
                if (c == '\\' && i + 1 < s.size()) {
                    cur += s[++i]; // Escape-Zeichen
                } else if (c == strChar) {
                    inStr = false;
                }
                continue;
            }
            if (c == '\'' || c == '"') {
                inStr = true; strChar = c; cur += c; continue;
            }
            if (c == '(' || c == '{' || c == '[') { depth++; cur += c; continue; }
            if (c == ')' || c == '}' || c == ']') { depth--; cur += c; continue; }
            if (c == ',' && depth == 0) {
                result.push_back(trim(cur)); cur.clear(); continue;
            }
            cur += c;
        }
        if (!cur.empty()) result.push_back(trim(cur));
        return result;
    }

    // Phase 27: alle (...)-Gruppen nach VALUES extrahieren
    // Phase 56: berücksichtigt jetzt JSON-Nesting ({...}, [...])
    static std::vector<std::vector<std::string>> parseValueGroups(const std::string& input) {
        // "VALUES" (case-insensitive) suchen
        std::string up = input;
        for (char& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        auto vpos = up.find("VALUES");
        if (vpos == std::string::npos) return {};

        std::vector<std::vector<std::string>> groups;
        size_t pos = vpos + 6;  // nach "VALUES"

        while (pos < input.size()) {
            // Leerzeichen und Kommas zwischen Gruppen überspringen
            while (pos < input.size() &&
                   (input[pos] == ' ' || input[pos] == '\t' ||
                    input[pos] == '\r' || input[pos] == '\n' ||
                    input[pos] == ','))
                ++pos;

            if (pos >= input.size() || input[pos] != '(') break;
            ++pos;  // '(' überspringen

            // Phase 56: Inhalt lesen mit Nesting-Bewusstsein
            // Track {}, [], () und Strings innerhalb der Werte
            std::string content;
            int depth = 0;
            bool inStr = false; char strChar = 0;
            while (pos < input.size()) {
                char c = input[pos];
                if (inStr) {
                    content += c;
                    if (c == '\\' && pos + 1 < input.size()) {
                        content += input[++pos];
                    } else if (c == strChar) {
                        inStr = false;
                    }
                    ++pos; continue;
                }
                if (c == '\'' || c == '"') {
                    inStr = true; strChar = c; content += c; ++pos; continue;
                }
                if (c == '{' || c == '[' || c == '(') { depth++; content += c; ++pos; continue; }
                if (c == '}' || c == ']') { depth--; content += c; ++pos; continue; }
                if (c == ')' && depth == 0) { ++pos; break; }  // Ende der Gruppe
                if (c == ')') { depth--; content += c; ++pos; continue; }
                content += c; ++pos;
            }

            groups.push_back(splitValues(content));
        }
        return groups;
    }

    static std::vector<std::string> splitTrim(const std::string& s, char delim) {
        std::vector<std::string> result;
        std::string cur;
        for (char c : s) {
            if (c == delim) { result.push_back(trim(cur)); cur.clear(); }
            else { cur += c; }
        }
        if (!cur.empty()) result.push_back(trim(cur));
        return result;
    }

    static std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    static std::string toUpper(std::string s) {
        for (char& c : s)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return s;
    }
};

} // namespace milansql
