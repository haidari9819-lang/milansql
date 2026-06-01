# Changelog

All notable changes to MilanSQL are documented here.

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
