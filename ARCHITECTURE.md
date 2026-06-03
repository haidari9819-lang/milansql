# MilanSQL Architecture

## Query Lifecycle

```
User SQL → Parser → ParsedCommand → Optimizer →
Executor → Storage → QueryResult
```

Every SQL statement flows through this pipeline:

1. **Parser** (`src/parser/parser.hpp`) — tokenizes input, builds `ParsedCommand`
2. **Dispatcher** (`src/dispatch.hpp`) — routes to the correct engine method
3. **Optimizer** (`src/optimizer/`) — rewrites queries, chooses join strategies
4. **Executor** — runs JOINs, aggregations, window functions
5. **Storage** (`src/storage/storage.hpp`) — binary serialization to `.milan` files

## Storage Layout

| File | Purpose |
|------|---------|
| `database.milan` | Main data file (binary, v9 page format) |
| `database.milan.wal` | Write-ahead log for crash recovery |
| `database.checkpoint` | Last checkpoint LSN |
| `database.config` | Runtime configuration |
| `database.stats` | Table statistics for the optimizer |
| `*.mdb` | Per-table page files (8KB pages) |

## Key Design Decisions

### Why header-only?
Single compilation unit means easier integration, no linker issues,
and a simpler build system. All engine logic lives in `.hpp` files —
`main.cpp` is the only `.cpp` that matters.

### Why C++17?
`std::shared_mutex` (reader-writer lock), `std::optional`,
`std::string_view`, and structured bindings cover all needs.
No C++20 required — maximum compiler compatibility.

### Why zero external dependencies?
- **Portability** — compiles anywhere with a C++17 compiler
- **Security** — no supply chain risk
- **Simplicity** — `cmake -B build && ninja -C build` is the entire build

## Component Map

```
src/
├── engine/engine.hpp        Core engine: Table, Row, Column, MVCC
├── parser/parser.hpp        SQL tokenizer + parser → ParsedCommand
├── dispatch.hpp             Command router → engine methods
├── storage/storage.hpp      Binary page format (v9) serializer
├── storage/column_store.hpp Column-oriented store for OLAP
├── optimizer/               Cost-based query optimizer + rewriter
├── server/
│   ├── http_server.hpp      REST API + Web Dashboard (port 8080)
│   ├── pg_server.hpp        PostgreSQL wire protocol v3 (port 5433)
│   ├── mysql_server.hpp     MySQL wire protocol (port 4407)
│   └── websocket_server.hpp WebSocket server
├── api/graphql_server.hpp   GraphQL API (port 8081)
├── search/
│   ├── bm25.hpp             BM25 relevance scorer
│   └── boolean_mode.hpp     Boolean full-text search parser
├── types/
│   ├── array_type.hpp       Array data type utilities
│   └── vector_type.hpp      VECTOR(n) type + HNSW index
├── compression/             LZ4 + RLE + Dictionary + ZSTD
├── replication/             Physical master/slave + logical pub/sub
├── cdc/cdc_manager.hpp      Change Data Capture
├── cache/statement_cache.hpp LRU prepared statement cache
├── pool/connection_pool.hpp  Connection pool multiplexer
├── pubsub/                  LISTEN/NOTIFY messaging
├── locking/                 Lock manager (SELECT FOR UPDATE)
├── cursor/server_cursor.hpp Server-side cursor support
├── profiler/slow_query_log.hpp Slow query log + fingerprinting
├── fdw/                     Foreign Data Wrapper (CSV + HTTP)
├── timeseries/              Time-series + time_bucket()
├── extensions/              Extension system + built-ins
└── main.cpp                 REPL + CLI entry point
```

## Concurrency Model

- **Per-table `std::shared_mutex`** — shared reads, exclusive writes (Phase 112)
- **MVCC** — each transaction sees a snapshot; readers never block writers
- **WAL** — all writes go to the WAL before the data file (Phase 114)
- **Atomic counters** — statistics updated lock-free

## Network Protocols

| Protocol | Port | Library |
|----------|------|---------|
| Native TCP | 4406 | Custom binary framing |
| MySQL Wire | 4407 | MySQL protocol v10 |
| PostgreSQL Wire | 5433 | PG protocol v3 |
| REST/HTTP | 8080 | Built-in HTTP server |
| GraphQL | 8081 | Built-in GraphQL engine |

## Testing

```bash
./build/milansql_tests.exe     # 285 unit/integration tests
./build/milansql_stress.exe    # stress tests
```

Tests cover: SQL correctness, MVCC, window functions, triggers,
stored procedures, replication, compression, BM25, cursors,
pgvector, slow query log, and more.
