#!/usr/bin/env python3
"""MilanSQL Performance Benchmark — P2/P3/P4 optimizations"""

import time
import json
import subprocess
import threading
import random
import sys
import statistics

BASE = "http://localhost:8080"

def get_token():
    r = subprocess.run([
        "curl", "-s", "-X", "POST", f"{BASE}/auth/login",
        "-H", "Content-Type: application/json",
        "-d", json.dumps({"username": "root", "password": "xy6NkX23qbieDprDPg2/ZX5zQcsn6Lyq"})
    ], capture_output=True, text=True)
    return json.loads(r.stdout)["token"]

def query(sql, token, timeout=120):
    r = subprocess.run([
        "curl", "-s", "-X", "POST", f"{BASE}/api/query",
        "-H", "Content-Type: application/json",
        "-H", f"Authorization: Bearer {token}",
        "-d", json.dumps({"sql": sql}),
        "--max-time", str(timeout)
    ], capture_output=True, text=True, timeout=timeout+10)
    try:
        return json.loads(r.stdout)
    except:
        return {"error": r.stdout[:200], "stderr": r.stderr[:200]}

def timed_query(sql, token, runs=5, timeout=120):
    """Run a query multiple times, return avg time in ms."""
    times = []
    result = None
    for i in range(runs):
        t0 = time.time()
        result = query(sql, token, timeout=timeout)
        elapsed = (time.time() - t0) * 1000
        times.append(elapsed)
    avg = statistics.mean(times)
    median = statistics.median(times)
    return avg, median, times, result

# ── MAIN ──
print("=" * 70)
print("MilanSQL Performance Benchmark — P2 / P3 / P4")
print("=" * 70)

TOKEN = get_token()
print(f"Token obtained. Running benchmarks...\n")

results = []

# ─────────────────────────────────────────
# P2: Read Concurrency (shared_mutex)
# ─────────────────────────────────────────
print("[P2] Read Concurrency — 50 concurrent SELECTs")

# Create a table to query
query("DROP TABLE IF EXISTS bench_concurrent", TOKEN)
query("CREATE TABLE bench_concurrent (id INT, val TEXT)", TOKEN)
for i in range(100):
    query(f"INSERT INTO bench_concurrent VALUES ({i}, 'row_{i}')", TOKEN)

errors_list = []
lock = threading.Lock()

def do_select(idx, token, timings, errs):
    sql = "SELECT COUNT(*) FROM bench_concurrent"
    t0 = time.time()
    r = query(sql, token)
    elapsed = (time.time() - t0) * 1000
    with lock:
        timings.append(elapsed)
        if isinstance(r, dict) and "error" in r and r.get("success") != True:
            errs.append(r)

# Warm-up
query("SELECT COUNT(*) FROM bench_concurrent", TOKEN)

# Run 50 concurrent — 3 rounds
best_wall = None
best_rps = 0
for run_num in range(3):
    timings = []
    errs = []
    threads = []
    t_start = time.time()
    for i in range(50):
        t = threading.Thread(target=do_select, args=(i, TOKEN, timings, errs))
        threads.append(t)
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=30)
    wall_time = (time.time() - t_start) * 1000

    rps = 50 / (wall_time / 1000) if wall_time > 0 else 0
    avg_latency = statistics.mean(timings) if timings else 0
    p99 = sorted(timings)[int(len(timings)*0.99)] if timings else 0

    print(f"  Run {run_num+1}: wall={wall_time:.0f}ms, RPS={rps:.0f}, avg_lat={avg_latency:.0f}ms, p99={p99:.0f}ms, errors={len(errs)}")
    if best_wall is None or wall_time < best_wall:
        best_wall = wall_time
        best_rps = rps
        best_avg_lat = avg_latency
        best_p99 = p99
        best_errs = len(errs)

results.append(("P2: 50 concurrent SELECTs", f"wall={best_wall:.0f}ms, RPS={best_rps:.0f}", f"avg_lat={best_avg_lat:.0f}ms, p99={best_p99:.0f}ms, {best_errs} errors"))

# Mixed tables concurrency
print("  Testing mixed-table concurrency...")
query("DROP TABLE IF EXISTS bench_conc2", TOKEN)
query("CREATE TABLE bench_conc2 (id INT, name TEXT)", TOKEN)
for i in range(50):
    query(f"INSERT INTO bench_conc2 VALUES ({i}, 'name_{i}')", TOKEN)

def do_mixed_select(idx, token, timings):
    table = "bench_concurrent" if idx % 2 == 0 else "bench_conc2"
    sql = f"SELECT COUNT(*) FROM {table}"
    t0 = time.time()
    r = query(sql, token)
    elapsed = (time.time() - t0) * 1000
    with lock:
        timings.append(elapsed)

timings_mixed = []
t_start = time.time()
threads = []
for i in range(50):
    t = threading.Thread(target=do_mixed_select, args=(i, TOKEN, timings_mixed))
    threads.append(t)
for t in threads:
    t.start()
for t in threads:
    t.join(timeout=30)
wall_mixed = (time.time() - t_start) * 1000
rps_mixed = 50 / (wall_mixed / 1000) if wall_mixed > 0 else 0
print(f"  Mixed-table: wall={wall_mixed:.0f}ms, RPS={rps_mixed:.0f}")
results.append(("P2: 50 mixed-table SELECTs", f"wall={wall_mixed:.0f}ms, RPS={rps_mixed:.0f}", "2 tables, concurrent reads"))


# ─────────────────────────────────────────
# P3: B-Tree Range Scans
# ─────────────────────────────────────────
print("\n[P3] B-Tree Range Scans — 100k rows")
print("  Creating bench_range table with 100k rows...")
query("DROP TABLE IF EXISTS bench_range", TOKEN)
query("CREATE TABLE bench_range (id INT, price DECIMAL, name TEXT)", TOKEN)

# Batch insert in chunks
TOTAL_ROWS = 100000
BATCH = 500
for batch_start in range(0, TOTAL_ROWS, BATCH):
    values = ", ".join(
        f"({i}, {random.uniform(0, 1000):.2f}, 'item_{i}')"
        for i in range(batch_start, min(batch_start + BATCH, TOTAL_ROWS))
    )
    r = query(f"INSERT INTO bench_range VALUES {values}", TOKEN, timeout=120)
    if batch_start % 20000 == 0:
        print(f"    Inserted {batch_start}/{TOTAL_ROWS}...")

print(f"    Inserted {TOTAL_ROWS}/{TOTAL_ROWS}. Creating index...")
r = query("CREATE INDEX idx_bench_price ON bench_range(price)", TOKEN, timeout=120)
print(f"    Index created: {json.dumps(r)[:100]}")

# Range scan (indexed)
print("  Benchmarking range scan (indexed)...")
avg, med, times, res = timed_query("SELECT COUNT(*) FROM bench_range WHERE price > 500", TOKEN, runs=5, timeout=60)
row_info = json.dumps(res.get("rows", res.get("data", "?")))[:80] if isinstance(res, dict) else "?"
print(f"    Range scan (price > 500): avg={avg:.1f}ms, median={med:.1f}ms — {row_info}")
results.append(("P3: Range scan (indexed, price>500)", f"avg={avg:.1f}ms, med={med:.1f}ms", row_info))

# Exact match (indexed)
print("  Benchmarking exact match (indexed)...")
avg2, med2, times2, res2 = timed_query("SELECT COUNT(*) FROM bench_range WHERE price = 250.00", TOKEN, runs=5, timeout=60)
row_info2 = json.dumps(res2.get("rows", res2.get("data", "?")))[:80] if isinstance(res2, dict) else "?"
print(f"    Exact match (price=250): avg={avg2:.1f}ms, median={med2:.1f}ms — {row_info2}")
results.append(("P3: Exact match (indexed, price=250)", f"avg={avg2:.1f}ms, med={med2:.1f}ms", row_info2))

# Full table scan (non-indexed column)
print("  Benchmarking full scan (non-indexed)...")
avg3, med3, times3, res3 = timed_query("SELECT COUNT(*) FROM bench_range WHERE name = 'item_5000'", TOKEN, runs=5, timeout=60)
row_info3 = json.dumps(res3.get("rows", res3.get("data", "?")))[:80] if isinstance(res3, dict) else "?"
print(f"    Full scan (name=...): avg={avg3:.1f}ms, median={med3:.1f}ms — {row_info3}")
results.append(("P3: Full scan (non-indexed, name=x)", f"avg={avg3:.1f}ms, med={med3:.1f}ms", row_info3))

# Range scan returning actual rows
print("  Benchmarking BETWEEN range scan...")
avg4, med4, times4, res4 = timed_query("SELECT * FROM bench_range WHERE price BETWEEN 100 AND 110", TOKEN, runs=3, timeout=60)
nrows = len(res4.get("rows", res4.get("data", []))) if isinstance(res4, dict) else "?"
print(f"    BETWEEN 100-110: avg={avg4:.1f}ms, median={med4:.1f}ms, rows={nrows}")
results.append(("P3: BETWEEN range (indexed)", f"avg={avg4:.1f}ms, med={med4:.1f}ms", f"{nrows} rows returned"))


# ─────────────────────────────────────────
# P4: O(n) DISTINCT
# ─────────────────────────────────────────
print("\n[P4] O(n) DISTINCT — 50k rows, low cardinality")
print("  Creating bench_distinct table with 50k rows...")
query("DROP TABLE IF EXISTS bench_distinct", TOKEN)
query("CREATE TABLE bench_distinct (id INT, status TEXT, category TEXT)", TOKEN)

STATUSES = ["active", "inactive", "pending", "closed", "archived",
            "suspended", "review", "approved", "rejected", "draft"]
CATEGORIES = ["A", "B", "C", "D", "E"]
TOTAL_D = 50000
BATCH_D = 500

for batch_start in range(0, TOTAL_D, BATCH_D):
    values = ", ".join(
        f"({i}, '{STATUSES[i % len(STATUSES)]}', '{CATEGORIES[i % len(CATEGORIES)]}')"
        for i in range(batch_start, min(batch_start + BATCH_D, TOTAL_D))
    )
    query(f"INSERT INTO bench_distinct VALUES {values}", TOKEN, timeout=120)
    if batch_start % 10000 == 0:
        print(f"    Inserted {batch_start}/{TOTAL_D}...")

print(f"    Inserted {TOTAL_D}/{TOTAL_D}.")

# Single-column DISTINCT
print("  Benchmarking DISTINCT (single column)...")
avg_d1, med_d1, _, res_d1 = timed_query("SELECT DISTINCT status FROM bench_distinct", TOKEN, runs=5, timeout=60)
nrows_d1 = len(res_d1.get("rows", res_d1.get("data", []))) if isinstance(res_d1, dict) else "?"
print(f"    DISTINCT status: avg={avg_d1:.1f}ms, med={med_d1:.1f}ms, distinct_values={nrows_d1}")
results.append(("P4: DISTINCT (1 col, 10 values)", f"avg={avg_d1:.1f}ms, med={med_d1:.1f}ms", f"{nrows_d1} distinct values from 50k rows"))

# Two-column DISTINCT
print("  Benchmarking DISTINCT (two columns)...")
avg_d2, med_d2, _, res_d2 = timed_query("SELECT DISTINCT status, category FROM bench_distinct", TOKEN, runs=5, timeout=60)
nrows_d2 = len(res_d2.get("rows", res_d2.get("data", []))) if isinstance(res_d2, dict) else "?"
print(f"    DISTINCT status,category: avg={avg_d2:.1f}ms, med={med_d2:.1f}ms, distinct_values={nrows_d2}")
results.append(("P4: DISTINCT (2 cols, 50 combos)", f"avg={avg_d2:.1f}ms, med={med_d2:.1f}ms", f"{nrows_d2} distinct combos from 50k rows"))

# UNION dedup
print("  Benchmarking UNION dedup...")
avg_d3, med_d3, _, res_d3 = timed_query(
    "SELECT status FROM bench_distinct UNION SELECT status FROM bench_distinct", TOKEN, runs=5, timeout=60)
nrows_d3 = len(res_d3.get("rows", res_d3.get("data", []))) if isinstance(res_d3, dict) else "?"
print(f"    UNION dedup: avg={avg_d3:.1f}ms, med={med_d3:.1f}ms, values={nrows_d3}")
results.append(("P4: UNION dedup (2x50k)", f"avg={avg_d3:.1f}ms, med={med_d3:.1f}ms", f"{nrows_d3} distinct values"))

# COUNT DISTINCT
print("  Benchmarking COUNT(DISTINCT ...)...")
avg_d4, med_d4, _, res_d4 = timed_query("SELECT COUNT(DISTINCT status) FROM bench_distinct", TOKEN, runs=5, timeout=60)
count_val = json.dumps(res_d4.get("rows", res_d4.get("data", "?")))[:60] if isinstance(res_d4, dict) else "?"
print(f"    COUNT(DISTINCT status): avg={avg_d4:.1f}ms, med={med_d4:.1f}ms — {count_val}")
results.append(("P4: COUNT(DISTINCT status)", f"avg={avg_d4:.1f}ms, med={med_d4:.1f}ms", count_val))

# ─────────────────────────────────────────
# Cleanup
# ─────────────────────────────────────────
print("\nCleaning up test tables...")
query("DROP TABLE IF EXISTS bench_concurrent", TOKEN)
query("DROP TABLE IF EXISTS bench_conc2", TOKEN)
query("DROP TABLE IF EXISTS bench_range", TOKEN)
query("DROP TABLE IF EXISTS bench_distinct", TOKEN)
print("Cleanup done.")

# ─────────────────────────────────────────
# Summary
# ─────────────────────────────────────────
print("\n" + "=" * 70)
print("BENCHMARK RESULTS SUMMARY")
print("=" * 70)
print(f"{'Test':<42} {'Time/Metric':<30} {'Notes'}")
print("-" * 110)
for test, metric, notes in results:
    print(f"{test:<42} {metric:<30} {notes}")
print("=" * 70)
