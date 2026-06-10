// ============================================================
// index_bench.cpp — 1M Row Index vs Full Scan Benchmark
// Phase 167: Production Index Performance Measurement
// ============================================================

#include <iostream>
#include <chrono>
#include <string>
#include <random>
#include <iomanip>
#include <vector>
#include <numeric>

#include "engine/engine.hpp"
#include "parser/parser.hpp"

int main() {
    std::cout << "========================================\n";
    std::cout << "  MilanSQL 100k Row Index Benchmark\n";
    std::cout << "========================================\n\n";

    milansql::Engine engine;
    milansql::Parser parser;

    constexpr int TOTAL_ROWS   = 100'000;
    constexpr int QUERY_COUNT  = 1000;

    // ── Schema ──────────────────────────────────────────────
    {
        auto cmd = parser.parse(
            "CREATE TABLE bench1m (id INT PRIMARY KEY, wert INT, name TEXT)");
        engine.createTable(cmd.tableName, cmd.columns, cmd.foreignKeys);
    }

    // ── 1. INSERT 1M Rows ───────────────────────────────────
    std::cout << "Inserting " << TOTAL_ROWS << " rows..." << std::flush;
    auto t0 = std::chrono::high_resolution_clock::now();

    for (int i = 1; i <= TOTAL_ROWS; ++i) {
        engine.insertRow("bench1m", {
            std::to_string(i),
            std::to_string(i * 7 % 999983),  // pseudo-random wert
            "item" + std::to_string(i)
        });
        if (i % 25000 == 0)
            std::cout << " " << (i / 1000) << "k" << std::flush;
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    long long insertMs = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::cout << "\n  100k INSERTs: " << insertMs << " ms ("
              << (TOTAL_ROWS * 1000LL / std::max(1LL, insertMs)) << " rows/sec)\n\n";

    // ── 2. Create Index on id ───────────────────────────────
    std::cout << "Creating BTREE index on id..." << std::flush;
    auto ti0 = std::chrono::high_resolution_clock::now();
    engine.createIndex("bench1m", {"id"}, "idx_id");
    auto ti1 = std::chrono::high_resolution_clock::now();
    std::cout << " " << std::chrono::duration_cast<std::chrono::milliseconds>(ti1 - ti0).count() << " ms\n\n";

    // ── 3. Prepare random IDs ───────────────────────────────
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> dist(1, TOTAL_ROWS);
    std::vector<int> randomIds(QUERY_COUNT);
    for (auto& id : randomIds) id = dist(rng);

    // ── 4. Indexed SELECT: WHERE id = ? ─────────────────────
    std::cout << "Running " << QUERY_COUNT << "x SELECT WHERE id = random (INDEXED)...\n";
    std::vector<double> indexedUs;
    indexedUs.reserve(QUERY_COUNT);

    for (int i = 0; i < QUERY_COUNT; ++i) {
        milansql::WhereCondition cond("id", "=", std::to_string(randomIds[i]));
        auto s = std::chrono::high_resolution_clock::now();
        auto result = engine.selectWhere("bench1m", {cond}, "AND");
        auto e = std::chrono::high_resolution_clock::now();
        double us = std::chrono::duration<double, std::micro>(e - s).count();
        indexedUs.push_back(us);
        (void)result;
    }

    double indexAvg = std::accumulate(indexedUs.begin(), indexedUs.end(), 0.0) / QUERY_COUNT;
    std::sort(indexedUs.begin(), indexedUs.end());
    double indexP50 = indexedUs[QUERY_COUNT / 2];
    double indexP99 = indexedUs[(int)(QUERY_COUNT * 0.99)];

    std::cout << std::fixed << std::setprecision(1);
    std::cout << "  Indexed AVG: " << indexAvg << " us\n";
    std::cout << "  Indexed P50: " << indexP50 << " us\n";
    std::cout << "  Indexed P99: " << indexP99 << " us\n\n";

    // ── 5. Full Scan SELECT: WHERE wert = ? (no index) ─────
    std::cout << "Running " << QUERY_COUNT << "x SELECT WHERE wert = random (FULL SCAN)...\n";
    std::vector<double> scanUs;
    scanUs.reserve(QUERY_COUNT);

    for (int i = 0; i < QUERY_COUNT; ++i) {
        int val = randomIds[i] * 7 % 999983;
        milansql::WhereCondition cond("wert", "=", std::to_string(val));
        auto s = std::chrono::high_resolution_clock::now();
        auto result = engine.selectWhere("bench1m", {cond}, "AND");
        auto e = std::chrono::high_resolution_clock::now();
        double us = std::chrono::duration<double, std::micro>(e - s).count();
        scanUs.push_back(us);
        (void)result;
    }

    double scanAvg = std::accumulate(scanUs.begin(), scanUs.end(), 0.0) / QUERY_COUNT;
    std::sort(scanUs.begin(), scanUs.end());
    double scanP50 = scanUs[QUERY_COUNT / 2];
    double scanP99 = scanUs[(int)(QUERY_COUNT * 0.99)];

    std::cout << "  Full Scan AVG: " << scanAvg << " us\n";
    std::cout << "  Full Scan P50: " << scanP50 << " us\n";
    std::cout << "  Full Scan P99: " << scanP99 << " us\n\n";

    // ── 6. Summary ──────────────────────────────────────────
    double speedup = scanAvg / std::max(0.001, indexAvg);
    std::cout << "========================================\n";
    std::cout << "  RESULTS (100k rows, " << QUERY_COUNT << " queries)\n";
    std::cout << "========================================\n";
    std::cout << "  INSERT:      " << insertMs << " ms total\n";
    std::cout << "  Indexed AVG: " << indexAvg << " us/query\n";
    std::cout << "  FullScan AVG:" << scanAvg << " us/query\n";
    std::cout << "  Speedup:     " << std::setprecision(0) << speedup << "x\n";
    std::cout << "========================================\n";

    return 0;
}
