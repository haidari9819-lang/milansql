// ============================================================
// durability_bench.cpp — Hardening-Audit Block 1
// Misst ehrliche INSERT-Raten MIT Durable Writes (WAL-fsync
// pro COMMIT), im Vergleich zur reinen In-Memory-Rate.
//
// Build (Server):
//   g++ -std=c++17 -O2 -I../ durability_bench.cpp -o /tmp/durability_bench -lpthread
// ============================================================

#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>

#include "engine/engine.hpp"



using namespace milansql;
using Clock = std::chrono::steady_clock;

static double msSince(Clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

int main() {
    const std::string wal = "bench_dur.wal";

    // ── 1) Durable Commits: 1 Row pro Transaktion ──────────────
    // BEGIN → INSERT → COMMIT (TX_COMMIT + CRC + fsync + dir-fsync)
    {
        Engine e;
        e.createTable("bench_t", {Column("id","INT"), Column("val","TEXT")});
        const int N = 2000;
        auto t0 = Clock::now();
        for (int i = 0; i < N; ++i) {
            e.beginTransaction(wal);
            e.insertRow("bench_t", {std::to_string(i), "v"});
            e.applyAndCommit();
            e.deleteWal();
        }
        double ms = msSince(t0);
        std::printf("1) durable single-row commits : %8.0f tx/sec  (%d txs, %.0f ms)\n",
                    N * 1000.0 / ms, N, ms);
    }
    std::remove(wal.c_str());

    // ── 2) Durable Commits: 1000 Rows pro Transaktion ──────────
    {
        Engine e;
        e.createTable("bench_t", {Column("id","INT"), Column("val","TEXT")});
        const int TXS = 50, ROWS = 1000;
        auto t0 = Clock::now();
        for (int t = 0; t < TXS; ++t) {
            e.beginTransaction(wal);
            for (int i = 0; i < ROWS; ++i)
                e.insertRow("bench_t", {std::to_string(t * ROWS + i), "v"});
            e.applyAndCommit();
            e.deleteWal();
        }
        double ms = msSince(t0);
        std::printf("2) durable batched (1000/tx)  : %8.0f rows/sec (%d rows, %.0f ms)\n",
                    TXS * ROWS * 1000.0 / ms, TXS * ROWS, ms);
    }
    std::remove(wal.c_str());

    // ── 3) Referenz: reine In-Memory-Inserts (alte 86k-Zahl) ───
    {
        Engine e;
        e.createTable("bench_t", {Column("id","INT"), Column("val","TEXT")});
        const int N = 100000;
        auto t0 = Clock::now();
        for (int i = 0; i < N; ++i)
            e.insertRow("bench_t", {std::to_string(i), "v"});
        double ms = msSince(t0);
        std::printf("3) in-memory (nicht durable)  : %8.0f rows/sec (%d rows, %.0f ms)\n",
                    N * 1000.0 / ms, N, ms);
    }
    return 0;
}
