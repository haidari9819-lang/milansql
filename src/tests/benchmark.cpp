// ============================================================
// benchmark.cpp — Performance-Benchmark für MilanSQL
// Phase 39.5: Production-Level Stabilization
// ============================================================

#include <iostream>
#include <chrono>
#include <string>
#include <cstdio>

#include "engine/engine.hpp"
#include "engine/btree.hpp"
#include "parser/parser.hpp"

int main() {
    std::cout << "========================================\n";
    std::cout << "  MilanSQL Benchmark (Phase 39.5)\n";
    std::cout << "========================================\n\n";

    milansql::Engine engine;
    milansql::Parser parser;

    // ── Schema anlegen ─────────────────────────────────────────
    {
        milansql::ParsedCommand cmd = parser.parse(
            "CREATE TABLE bench (id INT PRIMARY KEY AUTO_INCREMENT, val INT, name TEXT)");
        engine.createTable(cmd.tableName, cmd.columns, cmd.foreignKeys);
    }

    // ── 10.000 INSERTs ─────────────────────────────────────────
    auto t0 = std::chrono::high_resolution_clock::now();

    for (int i = 1; i <= 10000; ++i) {
        engine.insertRow("bench", {"NULL",
                                   std::to_string(i),
                                   "item" + std::to_string(i)});
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    long long insertMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

    // ── SELECT ohne Index (FULL SCAN) ───────────────────────────
    auto t2 = std::chrono::high_resolution_clock::now();

    {
        milansql::WhereCondition cond("val", "=", "9999");
        auto result = engine.selectWhere("bench", {cond}, "AND");
        (void)result;  // Ergebnis wird nicht gedruckt
    }

    auto t3 = std::chrono::high_resolution_clock::now();
    long long scanMs = std::chrono::duration_cast<std::chrono::milliseconds>(t3 - t2).count();

    // ── Index anlegen ───────────────────────────────────────────
    engine.createIndex("bench", {"val"}, "idx_val");

    // ── SELECT mit Index ────────────────────────────────────────
    auto t4 = std::chrono::high_resolution_clock::now();

    {
        milansql::WhereCondition cond("val", "=", "9999");
        auto result = engine.selectWhere("bench", {cond}, "AND");
        (void)result;
    }

    auto t5 = std::chrono::high_resolution_clock::now();
    long long indexMs = std::chrono::duration_cast<std::chrono::milliseconds>(t5 - t4).count();

    // ── Ergebnisse ausgeben ─────────────────────────────────────
    std::cout << "10.000 INSERTs:     " << insertMs << "ms\n";
    std::cout << "SELECT ohne Index:  " << scanMs   << "ms\n";
    std::cout << "SELECT mit Index:   " << indexMs  << "ms\n";

    if (indexMs > 0)
        std::cout << "Speedup:            "
                  << (static_cast<double>(scanMs) / static_cast<double>(indexMs))
                  << "x\n";
    else
        std::cout << "Speedup:            >>" << scanMs << "x (Index < 1ms)\n";

    std::cout << "\n";

    // ── Weitere Benchmarks ──────────────────────────────────────

    // 10.000 SELECTs mit Index
    auto t6 = std::chrono::high_resolution_clock::now();
    for (int i = 1; i <= 10000; ++i) {
        milansql::WhereCondition cond("val", "=", std::to_string(i));
        auto result = engine.selectWhere("bench", {cond}, "AND");
        (void)result;
    }
    auto t7 = std::chrono::high_resolution_clock::now();
    long long manySelectMs = std::chrono::duration_cast<std::chrono::milliseconds>(t7 - t6).count();
    std::cout << "10.000 Index-SELECTs: " << manySelectMs << "ms\n";

    // COUNT(*) over 10.000 rows
    auto t8 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 100; ++i) {
        std::size_t n = engine.countRows("bench");
        (void)n;
    }
    auto t9 = std::chrono::high_resolution_clock::now();
    long long countMs = std::chrono::duration_cast<std::chrono::milliseconds>(t9 - t8).count();
    std::cout << "100x COUNT(*):        " << countMs << "ms\n";

    std::cout << "\n========================================\n";
    std::cout << "  Benchmark abgeschlossen\n";
    std::cout << "========================================\n";

    return 0;
}
