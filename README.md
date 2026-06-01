# MilanSQL

## Try It Online

[Live Browser Demo](https://haidari9819-lang.github.io/milansql/demo.html) — Run SQL queries directly in your browser, no installation needed!

Features: Full SQL, table browser, example queries, query sharing, dark theme.

![Version](https://img.shields.io/badge/version-v4.0.0-gold)
![License](https://img.shields.io/badge/license-MIT-blue)
![Tests](https://img.shields.io/badge/tests-223%20passing-brightgreen)
![C++](https://img.shields.io/badge/C%2B%2B-17-blue)
![Dependencies](https://img.shields.io/badge/dependencies-0-brightgreen)

<!-- Topics: database sql cpp c-plus-plus query-engine btree replication mvcc window-functions postgresql-compatible -->

**From Zero to PostgreSQL Challenger — 100 phases, pure C++17**

> A complete relational database engine built from scratch in C++17.  
> 100 development phases. Zero external dependencies. 5 network protocols.

## Quick Start

```bash
# Build
cmake -B build -G Ninja && ninja -C build

# Start interactive REPL
./build/milansql.exe

# Start all servers
./build/milansql.exe --http --port 8080 --mysql --mysql-port 4407 --pg --pg-port 5433 --graphql --graphql-port 8081
```

## Feature Matrix

| Category | Features | Status |
|---|---|---|
| SQL Core | SELECT/INSERT/UPDATE/DELETE/TRUNCATE | ✅ |
| JOINs | INNER/LEFT/RIGHT/FULL, Hash/Merge/Nested Loop | ✅ |
| Subqueries | IN/EXISTS/Correlated/Scalar/Recursive CTE | ✅ |
| Window Functions | ROW_NUMBER/RANK/DENSE_RANK/SUM/AVG OVER PARTITION | ✅ |
| Transactions | MVCC/WAL/Savepoints/SELECT FOR UPDATE | ✅ |
| Constraints | PK/FK/NOT NULL/UNIQUE/CHECK/DEFAULT/AUTO_INCREMENT | ✅ |
| Data Types | INT/TEXT/REAL/DATE/TIME/DATETIME/JSON/POINT/Array | ✅ |
| Indexes | B-Tree/Composite/Full-Text/Spatial | ✅ |
| DDL | CREATE/DROP/ALTER TABLE/VIEW/INDEX/TRIGGER/PROCEDURE | ✅ |
| Procedures | Stored Procedures with Cursor/Loop/IF/SIGNAL | ✅ |
| Triggers | BEFORE/AFTER INSERT/UPDATE/DELETE (ROW + STATEMENT) | ✅ |
| Views | Regular Views + Materialized Views | ✅ |
| Partitioning | RANGE/LIST/HASH + Partition Pruning | ✅ |
| Replication | Physical Master/Slave + Logical Pub/Sub | ✅ |
| Protocols | TCP(4406) + MySQL(4407) + PG Wire(5433) + HTTP(8080) + GraphQL(8081) | ✅ |
| Security | Users/GRANT/REVOKE/Row-Level Security/Policies | ✅ |
| Performance | Hash Join/Buffer Pool/Query Cache/Parallel/Column Store | ✅ |
| Analytics | Window Functions/Column Store/Time-Series/time_bucket | ✅ |
| Admin | Backup/Restore/VACUUM/Checkpoint/CDC/Event Scheduler | ✅ |
| Extensions | milansql_math/crypto/uuid/text + Extension System | ✅ |
| Clients | Python DB-API 2.0 / Node.js / MySQL / psql | ✅ |
| Storage | Page-based I/O + Compression (LZ4/RLE/Dictionary) | ✅ |
| Catalog | INFORMATION_SCHEMA + pg_catalog (9 tables) | ✅ |
| Testing | 223 automated tests + Stress Testing | ✅ |

## Network Protocols

| Protocol | Port | Compatible With |
|---|---|---|
| Native TCP | 4406 | MilanSQL clients |
| MySQL Wire | 4407 | mysql CLI, Python mysql-connector, Node mysql2 |
| PostgreSQL Wire | 5433 | psql, libpq, psycopg2 |
| REST/HTTP API | 8080 | curl, any HTTP client, web browser |
| GraphQL | 8081 | GraphQL clients, web browser playground |

## SQL Examples

```sql
-- Window functions
SELECT name, salary,
       RANK() OVER (PARTITION BY dept ORDER BY salary DESC) AS rank
FROM employees;

-- Recursive CTE (Fibonacci)
WITH RECURSIVE fib(n, a, b) AS (
  SELECT 0, 0, 1
  UNION ALL
  SELECT n+1, b, a+b FROM fib WHERE n < 10
)
SELECT n, a FROM fib;

-- Array operations
SELECT array_agg(name) AS names FROM employees GROUP BY dept;
SELECT * FROM employees WHERE array_contains(skills, 'SQL');

-- Time-series
SELECT time_bucket(1 DAY, ts) AS day, AVG(value)
FROM metrics GROUP BY day ORDER BY day;

-- Change Data Capture
ALTER TABLE orders ENABLE CDC;
SELECT * FROM cdc.orders AFTER SEQUENCE 5;

-- Extensions
CREATE EXTENSION milansql_crypto;
SELECT md5('hello');  -- 5d41402abc4b2a76b9719d911017c592

-- pg_catalog
SELECT * FROM pg_catalog.pg_tables;
SELECT * FROM information_schema.columns WHERE table_name = 'employees';
```

## Architecture

```
src/
├── engine/engine.hpp          # Core engine: Table/Row/Column, all DML/DDL
├── parser/parser.hpp          # SQL tokenizer + parser → ParsedCommand
├── dispatch.hpp               # SQL command dispatcher
├── storage/storage.hpp        # Binary format v9 serializer
├── storage/column_store.hpp   # Column store engine (OLAP)
├── server/http_server.hpp     # REST API + Web Dashboard
├── server/pg_server.hpp       # PostgreSQL wire protocol v3
├── server/mysql_server.hpp    # MySQL wire protocol
├── api/graphql_server.hpp     # GraphQL API server
├── cdc/cdc_manager.hpp        # Change Data Capture
├── compression/               # LZ4/RLE/Dictionary/ZSTD compressors
├── timeseries/                # Time-series manager
├── extensions/                # Extension system + built-ins
├── pool/connection_pool.hpp   # Connection pool multiplexer
├── types/array_type.hpp       # Array type utilities
├── fdw/                       # Foreign Data Wrapper (CSV + HTTP)
├── copy/copy_manager.hpp      # COPY FROM/TO bulk import/export
├── cache/statement_cache.hpp  # LRU prepared statement cache
├── optimizer/                 # Query rewriter + adaptive stats
├── replication/               # Logical replication pub/sub
├── parallel/                  # Parallel query executor
├── pubsub/                    # LISTEN/NOTIFY pub/sub
├── locking/                   # Lock manager (SELECT FOR UPDATE)
└── main.cpp                   # REPL + CLI
```

## Building

**Requirements:** CMake 3.16+, C++17 compiler, MSYS2/UCRT64 (Windows) or GCC 10+ (Linux)

```bash
# Windows (MSYS2 UCRT64)
export PATH="/c/msys64/ucrt64/bin:$PATH"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build

# Linux
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run tests
./build/milansql_tests.exe   # 223 tests
./build/milansql_stress.exe  # stress tests
```

## Clients

```python
# Python
from milansql_client import connect
conn = connect(host='localhost', port=4406)
cur = conn.cursor()
cur.execute("SELECT * FROM users")
print(cur.fetchall())
```

```javascript
// Node.js
const { connect } = require('./clients/nodejs/milansql_client');
const conn = await connect({ host: 'localhost', port: 4406 });
const rows = await conn.query('SELECT * FROM users');
```

## Performance

- **INSERT throughput:** ~98,000 ops/sec
- **SELECT (no index):** ~1M rows/sec scan
- **Index lookup:** O(log n) B-Tree
- **Parallel queries:** up to N worker threads
- **Column store:** 5-10x faster aggregation for OLAP

## License

MIT License — see [LICENSE](LICENSE)
