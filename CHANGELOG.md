# Changelog

All notable changes to MilanSQL are documented here.

## [v8.2.0] — 2026-06-05 — "Production Ready — Stable Release"

### Phase 151: Zero Warning Build + Static Analysis
- Strict `-Wall -Wextra` build: **0 warnings** across all source files
- `SHOW MEMORY USAGE` — live engine metrics (tables, indexes, WAL entries, prepared transactions)
- Static analysis: raw pointer audit, file handle RAII verification, thread join/detach checks

### Phase 152: 10k Fuzz Test + Edge Case Hardening
- New standalone executable `milansql_fuzz_hardening` — 10,000 random queries across 5 categories
- **Syntax Chaos** (2000): garbage SQL, injections, half-formed queries, Unicode, NULL bytes
- **Type Chaos** (2000): overflow values, NULL arithmetic, wrong types, empty strings
- **Structure Chaos** (2000): non-existent tables/columns, circular CTEs, bad JOINs
- **Concurrency Chaos** (2000): 20 threads, no deadlocks
- **Recovery Chaos** (2000): orphaned BEGIN/COMMIT, PREPARE TRANSACTION cycles
- 19 edge case tests: 50-column tables, 1000-char strings, MAX INT32, rapid CREATE/DROP
- Chaos Monkey: 4 threads × 10 seconds, engine alive after all chaos

### Phase 149: Advanced Statistics + Optimizer Hints
- `ANALYZE TABLE` — collects row/page statistics per table
- `CREATE STATISTICS name ON col1, col2 FROM table` — multi-column statistics objects
- `SHOW STATISTICS [FOR table]` / `SHOW TABLE STATS [FOR table]`
- `SET enable_seqscan/enable_indexscan/enable_hashjoin/enable_nestloop = OFF/ON`

### Phase 150: User-Defined Functions + Stored Procedures V2
- `CREATE FUNCTION name(params) RETURNS type AS $$ body $$ LANGUAGE sql`
- `CREATE PROCEDURE name(params) AS $$ body $$ LANGUAGE sql`
- `CALL procedure_name(args)`
- `DROP FUNCTION` / `DROP PROCEDURE`
- `SHOW FUNCTIONS` / `SHOW PROCEDURES`

### Phase 147: Time-Series V2 + Continuous Aggregates
- `CREATE TABLE ... HYPERTABLE` — time-series optimized tables
- `CREATE CONTINUOUS AGGREGATE` with auto-refresh
- `time_bucket()`, `first()`, `last()` aggregate functions
- `add_retention_policy()`, `SHOW RETENTION POLICIES`
- `compress_chunk()`, `SHOW CHUNKS`

### Phase 148: Distributed Transactions + 2-Phase Commit
- `BEGIN DISTRIBUTED` / `PREPARE TRANSACTION xid` / `COMMIT PREPARED` / `ROLLBACK PREPARED`
- `SHOW PREPARED TRANSACTIONS`
- XA protocol: `XA START/END/PREPARE/COMMIT/ROLLBACK`
- `GET_LOCK()` / `RELEASE_LOCK()` / `IS_FREE_LOCK()`

### Phase 145: Advanced Security V2 + Audit Logging
- `SET audit_log = ON/OFF`, `SHOW AUDIT LOG [LIMIT n]`, `SHOW AUDIT LOG WHERE field = value`
- `SET audit_log_file = path`, `SET password_min_length`, `SET password_require_special`
- `SET allow_host`, `SET deny_host`, `SET blacklist_query`
- `SET max_connections_per_ip`, `SET connection_rate_limit`

### Phase 146: Online DDL + Zero-Downtime Schema Changes
- `CREATE INDEX CONCURRENTLY` — non-blocking index builds
- `BEGIN DDL TRANSACTION` / `COMMIT DDL TRANSACTION` / `ROLLBACK DDL TRANSACTION`
- `SHOW DDL HISTORY`, `SHOW SCHEMA VERSION`
- `ALTER TABLE ... ADD COLUMN ... DEFAULT value`

**636/636 tests pass. Zero external dependencies. Production Ready.**

---

## [v7.6.0] — 2026-06-04 — "LATERAL + JSONB Complete"

### Fixed in Phase 138–139

- **Phase 138 LATERAL JOIN:** Fixed parser bug where `table.column` references in LATERAL subqueries were tokenized with spaces (`u2 . id`), breaking correlated substitution. LATERAL JOINs now correctly re-execute the subquery for each outer row.
- **Phase 139 JSONB_AGG:** Detected as aggregate function and routed through `parseGroupByQuery` with implicit whole-table aggregation via `globalAgg()`. Returns proper JSON array with all values.
- **Phase 139 JSONB_BUILD_OBJECT:** Added to SFUNCS routing list and `parseSelectFull`; now evaluated as scalar function per-row via `projectWithItems`, returning one JSON object per row.
- **dispatch.hpp compile errors:** Fixed 3 compile errors in the server binary — deleted Table copy constructor usage (→ `std::move`), wrong `dispatch()` return type (→ `dispatch_executeSelectToTable`).
- 494 automated tests (26 new tests)

---

## [v7.5.0] — 2026-06-03 — "Advanced SQL Analytics"

### Added since v7.0.0

- **Phase 136:** GROUPING SETS / ROLLUP / CUBE — multi-dimensional aggregation with super-aggregate rows
- **Phase 137:** TABLESAMPLE BERNOULLI(n) — random row sampling + DISTINCT ON (col1, col2) — PostgreSQL-compatible first-row-per-group
- 468 automated tests (42 new tests)

---

## [v7.0.0] — 2026-06-02 — "Enterprise Ready"

### 130 Phases Complete

The most feature-complete open-source SQL database engine built from scratch in C++17.
130 development phases. 370 automated tests. Zero external dependencies.

### Added since v6.0.0

- **Phase 120:** Slow Query Log + Query Fingerprinting + Top Queries Analysis
- **Phase 121:** pgvector V2 + Semantic Search API + Vector Aggregations
- **Phase 122:** Clean Git History + Professional Release Branch
- **Phase 123:** ARCHITECTURE.md + Engine Code Documentation
- **Phase 124:** docs/internals.html — Technical Deep Dive
- **Phase 125:** Load Balancer + Adaptive Connection Routing (Read/Write Split)
- **Phase 126:** Plan Cache V2 + Query Hints + Optimizer Trace
- **Phase 127:** Multi-Tenant Support (SaaS-ready isolation)
- **Phase 128:** High Availability + Auto-Failover Sentinel
- **Phase 129:** Connection String V2 + Service Discovery + DSN
- **Phase 130:** v7.0.0 Release — Enterprise Ready

---

## [v5.8.0] — 2026-05-30

### Added

- **Phase 110:** SSL/TLS Encryption (SChannel on Windows, OpenSSL on Linux)
- **Phase 111:** pgvector AI/ML — HNSW index, Cosine/L2/Inner-Product distance, vector_similarity()
- **Phase 112:** Lock-free B-Tree + per-table Reader-Writer Locks (shared reads, exclusive writes)
- **Phase 113:** DP Query Planner V2 — dynamic-programming join order optimizer + column histograms
- **Phase 114:** Crash Recovery V2 — Double-Write Buffer (CRC-32) + LSN Manager + CHECK/REPAIR TABLE
- **Phase 115:** Production Packaging — Kubernetes CRD + Helm chart + APT/RPM packages + Windows MSI + GitHub Actions release pipeline
- **Phase 116:** Multi-Model Database — Document Store (MongoDB-like JSON collections) + Graph Store (Neo4j-like property graph with BFS)
- **Phase 117:** Embedded Mode — SQLite-compatible C API (`milansql_open/exec/prepare/step`), static library, single-header amalgamation

---

## [v4.0.0] — 2026-05-31 — "The PostgreSQL Challenger"

### 100 Phases Complete

A complete relational database engine built from scratch in C++17 across 100 development phases.

### Added in v3.x → v4.0.0

- **Phase 88:** Array Data Type (TEXT[], INT[], array_agg, UNNEST)
- **Phase 89:** Foreign Data Wrapper (CSV FDW + HTTP/JSON FDW)
- **Phase 90:** Extension System (milansql_math/crypto/uuid/text)
- **Phase 91:** PostgreSQL Wire Protocol (psql + libpq compatible)
- **Phase 92:** COPY FROM/TO (Bulk Import/Export CSV + Binary)
- **Phase 93:** Prepared Statement Cache (LRU) + Statement-Level Triggers
- **Phase 94:** Connection Pool Multiplexing (Session/Transaction/Statement mode)
- **Phase 95:** pg_catalog Compatibility + psql Meta-Commands (\dt \d \l \du)
- **Phase 96:** Data Compression (LZ4 + RLE + Dictionary + ZSTD)
- **Phase 97:** Time-Series Optimizations (time_bucket + Retention Policies)
- **Phase 98:** Change Data Capture (CDC) + GraphQL API (Port 8081)
- **Phase 99:** Extended Test Suite (223 tests) + Stress Testing
- **Phase 100:** v4.0.0 Release — Polish, README, CHANGELOG

### Core Features (Phases 1–87)

- Full SQL engine: SELECT/INSERT/UPDATE/DELETE with all clauses
- B-Tree indexes, composite indexes, full-text search, spatial indexes
- MVCC transactions, WAL, crash recovery, savepoints
- Window functions, CTEs, recursive CTEs
- Stored procedures with cursors, loops, IF/CASE, SIGNAL
- BEFORE/AFTER triggers (ROW + STATEMENT granularity)
- Regular and materialized views
- Table partitioning (RANGE/LIST/HASH)
- Physical and logical replication
- Buffer pool manager, query cache, parallel query execution
- Column store engine for OLAP workloads
- Row-Level Security (RLS) policies
- LISTEN/NOTIFY pub/sub messaging
- JSON functions, POINT/spatial, REGEXP
- MySQL Wire Protocol compatibility
- REST API + Web Dashboard
- Python DB-API 2.0 client + Node.js client
- Page-based binary storage format (v9)
- Query optimizer with adaptive statistics
- Event scheduler

## [v3.9.0]
- Phase 96: Data Compression
- Phase 97: Time-Series

## [v3.8.0]
- Phase 94: Connection Pool
- Phase 95: pg_catalog

## [v3.7.0]
- Phase 92: COPY FROM/TO
- Phase 93: Statement Cache + Statement Triggers

## [v3.6.0]
- Phase 90: Extension System
- Phase 91: PostgreSQL Wire Protocol

## [v3.5.0]
- Phase 88: Array Data Type
- Phase 89: Foreign Data Wrapper
