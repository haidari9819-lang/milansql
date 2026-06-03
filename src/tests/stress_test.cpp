// ============================================================
// stress_test.cpp — MilanSQL Stress Test (Phase 99 + Phase 133)
// ============================================================

#include <iostream>
#include <chrono>
#include <string>
#include <vector>
#include <stdexcept>
#include <thread>
#include <atomic>

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

    // ── Phase 133 Test 8: Memory Stability ──────────────────────
    {
        Engine eng8;
        Parser par8;
        execSQL(eng8, par8, "CREATE TABLE mem_test (id INT PRIMARY KEY, v TEXT)");
        for (int i = 0; i < 100; i++) {
            execSQL(eng8, par8, "INSERT INTO mem_test VALUES (" + std::to_string(i) + ", val" + std::to_string(i) + ")");
        }
        // Run 100 selects — memory should be stable (no crash)
        for (int i = 0; i < 100; i++) {
            auto& t = eng8.selectAll("mem_test");
            (void)t;
        }
        std::cout << "Test 8: Memory stability (100 INSERTs + 100 SELECTs): PASS\n";
    }

    // ── Phase 133 Test 9: Concurrent 20 threads ─────────────────
    {
        Engine eng9;
        Parser par9;
        execSQL(eng9, par9, "CREATE TABLE concurrent_test (id INT PRIMARY KEY, val INT)");
        std::vector<std::thread> threads;
        std::atomic<int> errors{0};
        std::atomic<int> counter{0};
        for (int t = 0; t < 20; t++) {
            threads.emplace_back([&, t]() {
                for (int i = 0; i < 50; i++) {
                    try {
                        int id = counter.fetch_add(1);
                        eng9.insertRow("concurrent_test", {std::to_string(id), std::to_string(t*100+i)});
                        auto& tbl = eng9.selectAll("concurrent_test");
                        (void)tbl;
                    } catch(...) { errors++; }
                }
            });
        }
        for (auto& th : threads) th.join();
        std::cout << "Test 9: Concurrent 20 threads (20x50 ops), errors=" << errors.load() << "\n";
        if (errors.load() > 0) {
            std::cerr << "FAIL: concurrent test had " << errors.load() << " errors\n";
            return 1;
        }
    }

    // ── Phase 133 Test 10: Transaction Stress ───────────────────
    {
        Engine eng10;
        Parser par10;
        execSQL(eng10, par10, "CREATE TABLE tx_test (id INT PRIMARY KEY, val INT)");
        int committed = 0, rolledBack = 0;
        for (int i = 0; i < 100; i++) {
            try {
                execSQL(eng10, par10, "BEGIN");
                execSQL(eng10, par10, "INSERT INTO tx_test VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")");
                if (i % 2 == 0) {
                    execSQL(eng10, par10, "COMMIT");
                    committed++;
                } else {
                    execSQL(eng10, par10, "ROLLBACK");
                    rolledBack++;
                }
            } catch (...) {
                try { eng10.rollbackTransaction(); } catch (...) {}
            }
        }
        auto& tbl10 = eng10.selectAll("tx_test");
        int count = (int)tbl10.rowCount();
        std::cout << "Test 10: Transaction stress, committed=" << committed
                  << ", rolledBack=" << rolledBack << ", rows=" << count << "\n";
        if (count != committed) {
            std::cerr << "FAIL: expected " << committed << " rows, got " << count << "\n";
            return 1;
        }
    }

    // ── Phase 133 Test 11: Fuzzing (no crash) ───────────────────
    {
        Engine eng11;
        Parser par11;
        execSQL(eng11, par11, "CREATE TABLE fuzz_tbl (id INT, val TEXT)");
        const std::vector<std::string> templates = {
            "SELECT * FROM fuzz_tbl",
            "INSERT INTO fuzz_tbl VALUES (1, val)",
            "SELECT * FROM fuzz_tbl WHERE id = 1",
            "DELETE FROM fuzz_tbl WHERE id = 999",
            "UPDATE fuzz_tbl SET val = y WHERE id = 1",
        };
        for (int i = 0; i < 100; i++) {
            try {
                execSQL(eng11, par11, templates[i % templates.size()]);
            } catch (...) {
                // exceptions are fine, crashes are not
            }
        }
        std::cout << "Test 11: Fuzzing (100 mixed queries, no crash): PASS\n";
    }

    // ── Phase 133 Test 12: Large data (10k rows) ─────────────────
    {
        Engine eng12;
        Parser par12;
        execSQL(eng12, par12, "CREATE TABLE large_tbl (id INT PRIMARY KEY, val INT, name TEXT)");
        auto t0 = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < 10000; i++) {
            execSQL(eng12, par12, "INSERT INTO large_tbl VALUES (" + std::to_string(i) +
                ", " + std::to_string(i) + ", row" + std::to_string(i) + ")");
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double,std::milli>(t1-t0).count();
        WhereCondition wc("val", ">", "5000");
        auto r = eng12.selectWhere("large_tbl", {wc}, "AND");
        int count = (int)r.table.rowCount();
        std::cout << "Test 12: Large data (10k rows) in " << (int)ms
                  << "ms, count(val>5000)=" << count << "\n";
        if (count != 4999) {
            std::cerr << "FAIL: expected 4999 rows with val>5000, got " << count << "\n";
            return 1;
        }
        if (ms >= 30000.0) {
            std::cerr << "FAIL: 10k inserts took " << ms << "ms (limit 30s)\n";
            return 1;
        }
    }

    // Cleanup temp WAL files
    std::remove("/tmp/stress_milansql.wal");

    std::cout << "\n=== Stress Test PASSED ===\n";
    return 0;
}
