# MilanSQL Architecture

## Query Lifecycle

```
                    ┌─────────────────────────────────────┐
                    │           User / Client              │
                    │  SQL string or network protocol      │
                    └──────────────────┬──────────────────┘
                                       │
                    ┌──────────────────▼──────────────────┐
                    │         Parser (parser.hpp)          │
                    │  Tokenizer → ParsedCommand struct    │
                    └──────────────────┬──────────────────┘
                                       │
                    ┌──────────────────▼──────────────────┐
                    │       Dispatcher (dispatch.hpp)      │
                    │  Routes command to engine method     │
                    └──────────┬───────────────┬──────────┘
                               │               │
              ┌────────────────▼──┐    ┌───────▼────────────────┐
              │   Optimizer       │    │   Executor              │
              │  Cost-based plan  │    │  Hash/Merge/NL JOINs    │
              │  Join order (DP)  │    │  Aggregations, Windows  │
              └────────────┬──────┘    └────────┬───────────────┘
                           │                    │
                    ┌──────▼────────────────────▼──────────┐
                    │         Engine (engine.hpp)           │
                    │  Table • MVCC • WAL • Buffer Pool     │
                    └──────────────────┬──────────────────┘
                                       │
                    ┌──────────────────▼──────────────────┐
                    │        Storage (storage.hpp)         │
                    │  Binary page format v9 (8 KB pages) │
                    └─────────────────────────────────────┘
```

## Source Tree

```
src/
├── engine/
│   └── engine.hpp           Core engine: Table, Row, Column, MVCC, WAL,
│                            Buffer Pool, Indexes, Aggregations, Triggers
├── parser/
│   └── parser.hpp           SQL tokenizer + recursive-descent parser
│                            Produces ParsedCommand struct
├── dispatch.hpp             Routes ParsedCommand → engine method
│                            Handles timing, slow query log
├── storage/
│   ├── storage.hpp          Binary serializer: page format v9
│   └── column_store.hpp     Column-oriented store for OLAP
├── optimizer/               Cost-based query optimizer
│                            Adaptive statistics + join order DP
├── server/
│   ├── http_server.hpp      REST API + Web Dashboard (port 8080)
│   ├── pg_server.hpp        PostgreSQL wire protocol v3 (port 5433)
│   ├── mysql_server.hpp     MySQL wire protocol v10 (port 4407)
│   └── websocket_server.hpp WebSocket server
├── api/
│   └── graphql_server.hpp   GraphQL API (port 8081)
├── search/
│   ├── bm25.hpp             BM25 relevance scorer (k1=1.5, b=0.75)
│   └── boolean_mode.hpp     Boolean full-text search parser
├── types/
│   ├── array_type.hpp       TEXT[] / INT[] array type
│   └── vector_type.hpp      VECTOR(n) type + HNSW index
├── compression/             LZ4 + RLE + Dictionary + ZSTD compressors
├── replication/             Physical master/slave + logical pub/sub
├── cdc/
│   └── cdc_manager.hpp      Change Data Capture (ALTER TABLE … ENABLE CDC)
├── cache/
│   └── statement_cache.hpp  LRU prepared statement cache
├── pool/
│   └── connection_pool.hpp  Session/Transaction/Statement mode pooling
├── pubsub/                  LISTEN/NOTIFY pub-sub messaging
├── locking/                 Lock manager (SELECT FOR UPDATE)
├── cursor/
│   └── server_cursor.hpp    Server-side cursor (DECLARE/OPEN/FETCH)
├── profiler/
│   └── slow_query_log.hpp   Slow query log + SQL fingerprinting
├── fdw/                     Foreign Data Wrappers: CSV + HTTP/JSON
├── timeseries/              Time-series: time_bucket(), retention policies
├── extensions/              Extension system + milansql_math/crypto/uuid/text
└── main.cpp                 REPL + CLI entry point
```

## Key Design Decisions

### Header-only Engine
All engine logic lives in `.hpp` files. `main.cpp` is the only translation unit
that includes the engine. Benefits: single compilation unit, no linker issues,
trivially embeddable in other projects.

### Zero External Dependencies
No third-party libraries. Everything — B-Tree indexes, MVCC, WAL, BM25,
HNSW vector search, LZ4 compression, wire protocols — is implemented from scratch.
Build with one command: `cmake -B build && ninja -C build`.

### MVCC (Multi-Version Concurrency Control)
Each row carries `xmin` (created by transaction ID) and `xmax` (deleted by
transaction ID). Readers see a consistent snapshot without locking writers.
Vacuum reclaims rows where `xmax` is below the oldest active transaction.

### WAL (Write-Ahead Log)
Every write is appended to the WAL before touching data pages. On crash:
- Replay WAL from last checkpoint to restore committed transactions
- Discard uncommitted entries
Double-write buffer (Phase 114) prevents torn-page writes.

### Per-table Reader-Writer Lock
`std::shared_mutex` per table (Phase 112):
- `shared_lock` for SELECT — multiple readers run concurrently
- `unique_lock` for INSERT/UPDATE/DELETE — exclusive write access
Combined with MVCC this gives full snapshot isolation.

### Join Strategy Selection
| Condition | Strategy | Complexity |
|-----------|----------|------------|
| Both sides ≤ 10 rows | Nested Loop | O(n × m) |
| No usable index | Hash Join | O(n + m) |
| Both sides have index | Merge Join | O(n log n) |
| 3+ tables | DP planner | O(2ⁿ × n²) |

### BM25 Full-Text Search
Okapi BM25 with k1 = 1.5, b = 0.75.
IDF = log((N − df + 0.5) / (df + 0.5) + 1)
TF  = (freq × (k1 + 1)) / (freq + k1 × (1 − b + b × dl/avgdl))
Boolean mode supports `+required`, `-excluded`, and `"phrase"` terms.

## Network Protocol Stack

```
┌────────┬───────────┬─────────────────┬───────┬──────────┐
│  Port  │ Protocol  │   Compatible     │ Auth  │ TLS      │
├────────┼───────────┼─────────────────┼───────┼──────────┤
│  4406  │ Native TCP│ MilanSQL clients │ Yes   │ Phase110 │
│  4407  │ MySQL Wire│ mysql, pymysql   │ Yes   │ Phase110 │
│  5433  │ PG Wire v3│ psql, psycopg2   │ Yes   │ Phase110 │
│  8080  │ HTTP REST │ curl, browser    │ Token │ Phase110 │
│  8081  │ GraphQL   │ GraphQL clients  │ Token │ Phase110 │
└────────┴───────────┴─────────────────┴───────┴──────────┘
```

## Storage File Format

| File | Format | Purpose |
|------|--------|---------|
| `*.milan` | Binary v9, 8 KB pages | Main data |
| `*.milan.wal` | Sequential log entries | Crash recovery |
| `*.checkpoint` | 8-byte LSN | Last checkpoint |
| `*.config` | Key=value text | Runtime settings |
| `*.stats` | Binary histogram | Optimizer statistics |
| `*.mdb` | Page-based | Per-table page files |

Each data page layout:
```
[PageHeader 16B][Slot array]...[Row data (grows from end)]
PageHeader: magic(4) | lsn(8) | checksum(4)
```
