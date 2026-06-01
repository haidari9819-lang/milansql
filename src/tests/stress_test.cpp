// ============================================================
// stress_test.cpp — MilanSQL Stress Test (Phase 99)
// ============================================================

#include <iostream>
#include <chrono>
#include <string>
#include <vector>
#include <stdexcept>

#include "engine/engine.hpp"
#include "parser/parser.hpp"

using namespace milansql;

// ── Helper: execute SQL DDL/DML directly via engine ────────────
static void execSQL(Engine& engine, Parser& parser, const std::string& sql) {
    ParsedCommand cmd = parser.parse(sql);
    switch (cmd.type) {
        case CommandType::CREATE_TABLE:
            engine.createTable(cmd.tableName, cmd.columns, cmd.foreignKeys);
            break;
        case CommandType::INSERT: {
            const auto& rows = cmd.multiValues.empty()
                ? std::vector<std::vector<std::string>>{cmd.values}
                : cmd.multiValues;
            for (const auto& vals : rows)
                engine.insertRow(cmd.tableName, vals);
            break;
        }
        case CommandType::DELETE:
            if (!cmd.whereColumn.empty())
                engine.deleteWhere(cmd.tableName, cmd.whereColumn, cmd.whereValue);
            else
                engine.deleteAll(cmd.tableName);
            break;
        case CommandType::UPDATE:
            if (!cmd.whereColumn.empty())
                engine.updateWhere(cmd.tableName, cmd.updateCols, cmd.updateVals,
                                   cmd.whereColumn, cmd.whereValue);
            else
                engine.updateAll(cmd.tableName, cmd.updateCols, cmd.updateVals);
            break;
        case CommandType::BEGIN:
            engine.beginTransaction("/tmp/stress_milansql.wal");
            break;
        case CommandType::COMMIT:
            engine.applyAndCommit();
            break;
        case CommandType::ROLLBACK:
            engine.rollbackTransaction();
            break;
        case CommandType::CREATE_INDEX:
            engine.createIndex(cmd.tableName, cmd.indexColumns, cmd.indexName);
            break;
        case CommandType::DROP_TABLE:
            engine.dropTable(cmd.tableName);
            break;
        default:
            break;
    }
}

int main() {
    std::cout << "=== MilanSQL Stress Test ===\n\n";

    Engine engine;
    Parser parser;

    // ── Test 1: 10,000 sequential INSERTs ────────────────────────
    {
        execSQL(engine, parser, "CREATE TABLE stress_t (id INT, val TEXT)");

        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 10000; i++) {
            std::string sql = "INSERT INTO stress_t VALUES (" +
                std::to_string(i) + ", val" + std::to_string(i) + ")";
            execSQL(engine, parser, sql);
        }
        auto end = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();

        // Verify count
        auto& tbl = engine.selectAll("stress_t");
        std::size_t rowCount = tbl.rowCount();
        std::cout << "Test 1: 10,000 INSERTs in " << ms << "ms ("
                  << static_cast<int>(10000.0 / ms * 1000) << " ops/sec), rows="
                  << rowCount << "\n";
        if (rowCount != 10000) {
            std::cerr << "FAIL: expected 10000 rows, got " << rowCount << "\n";
            return 1;
        }
    }

    // ── Test 2: 1,000 selectAll calls ────────────────────────────
    {
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 1000; i++) {
            auto& t = engine.selectAll("stress_t");
            (void)t;
        }
        auto end = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();
        std::cout << "Test 2: 1,000 selectAll in " << ms << "ms ("
                  << static_cast<int>(1000.0 / ms * 1000) << " ops/sec)\n";
    }

    // ── Test 3: 1,000 transactions ───────────────────────────────
    {
        execSQL(engine, parser, "CREATE TABLE tx_t (id INT PRIMARY KEY, val TEXT)");

        auto start = std::chrono::steady_clock::now();
        int ok = 0;
        for (int i = 0; i < 1000; i++) {
            try {
                execSQL(engine, parser, "BEGIN");
                execSQL(engine, parser,
                    "INSERT INTO tx_t VALUES (" + std::to_string(i) + ", v)");
                execSQL(engine, parser, "COMMIT");
                ok++;
            } catch (...) {
                // If transaction already active or other error, try to rollback
                try { engine.rollbackTransaction(); } catch (...) {}
            }
        }
        auto end = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();
        std::cout << "Test 3: 1,000 transactions in " << ms << "ms, succeeded=" << ok << "\n";
        if (ok < 900) {
            std::cerr << "FAIL: expected at least 900 successful transactions, got " << ok << "\n";
            return 1;
        }
    }

    // ── Test 4: 1,000 WHERE queries ──────────────────────────────
    {
        auto start = std::chrono::steady_clock::now();
        int found = 0;
        for (int i = 0; i < 1000; i++) {
            int target = i % 10000;
            WhereCondition wc("id", "=", std::to_string(target));
            auto result = engine.selectWhere("stress_t", {wc}, "AND").table;
            if (result.rowCount() == 1) found++;
        }
        auto end = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();
        std::cout << "Test 4: 1,000 WHERE queries in " << ms << "ms, found=" << found << "\n";
    }

    // ── Test 5: Index creation + 1,000 indexed lookups ───────────
    {
        execSQL(engine, parser, "CREATE TABLE idx_stress (id INT, score INT, name TEXT)");
        for (int i = 0; i < 5000; i++) {
            execSQL(engine, parser,
                "INSERT INTO idx_stress VALUES (" + std::to_string(i) +
                ", " + std::to_string(i % 100) +
                ", user" + std::to_string(i) + ")");
        }
        execSQL(engine, parser, "CREATE INDEX idx_score ON idx_stress (score)");

        auto start = std::chrono::steady_clock::now();
        int total = 0;
        for (int i = 0; i < 1000; i++) {
            WhereCondition wc("score", "=", std::to_string(i % 100));
            auto result = engine.selectWhere("idx_stress", {wc}, "AND").table;
            total += static_cast<int>(result.rowCount());
        }
        auto end = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();
        std::cout << "Test 5: 1,000 indexed lookups in " << ms << "ms, total_rows=" << total << "\n";
    }

    // ── Test 6: 500 DELETE + re-insert cycles ─────────────────────
    {
        execSQL(engine, parser, "CREATE TABLE cycle_t (id INT, val TEXT)");
        auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < 500; i++) {
            execSQL(engine, parser,
                "INSERT INTO cycle_t VALUES (" + std::to_string(i) + ", data)");
            engine.deleteWhere("cycle_t", "id", std::to_string(i));
        }
        auto end = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();
        auto& tbl = engine.selectAll("cycle_t");
        std::cout << "Test 6: 500 insert+delete cycles in " << ms << "ms, remaining="
                  << tbl.rowCount() << "\n";
        if (tbl.rowCount() != 0) {
            std::cerr << "FAIL: expected 0 rows after delete cycles\n";
            return 1;
        }
    }

    // ── Test 7: Large GROUP BY ────────────────────────────────────
    {
        execSQL(engine, parser, "CREATE TABLE grp_t (cat INT, val INT)");
        for (int i = 0; i < 1000; i++) {
            execSQL(engine, parser,
                "INSERT INTO grp_t VALUES (" + std::to_string(i % 10) +
                ", " + std::to_string(i) + ")");
        }

        auto start = std::chrono::steady_clock::now();
        std::vector<SelectItem> items;
        SelectItem catItem; catItem.colName = "cat"; items.push_back(catItem);
        SelectItem cntItem; cntItem.isAgg = true; cntItem.aggFunc = "COUNT";
        cntItem.aggCol = "*"; items.push_back(cntItem);
        auto result = engine.groupBy("grp_t", {}, "AND", {"cat"}, items, {}, "AND");
        auto end = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(end - start).count();
        std::cout << "Test 7: GROUP BY on 1000 rows in " << ms << "ms, groups="
                  << result.rowCount() << "\n";
        if (result.rowCount() != 10) {
            std::cerr << "FAIL: expected 10 groups, got " << result.rowCount() << "\n";
            return 1;
        }
    }

    // Cleanup temp WAL files
    std::remove("/tmp/stress_milansql.wal");

    std::cout << "\n=== Stress Test PASSED ===\n";
    return 0;
}
