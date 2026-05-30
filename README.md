# MilanSQL

![Version](https://img.shields.io/badge/version-v3.0.0-gold)
![CI](https://github.com/haidari9819-lang/milansql/actions/workflows/ci.yml/badge.svg)
![License](https://img.shields.io/badge/license-MIT-blue)
![Language](https://img.shields.io/badge/language-C%2B%2B17-orange)
![Phases](https://img.shields.io/badge/phases-82-brightgreen)

> **A production-grade relational database engine built from scratch in C++17 — zero external dependencies.**

Developed by **Mirwais Haidari**, built phase by phase from a blank file to a full-featured database server. Every byte of SQL parsing, query optimization, transaction management, replication, and network protocol is hand-written C++17.

---

## What's New in v3.0.0

| Phase | Feature |
|-------|---------|
| **79** | **Connection String Parser** — `milansql://user:pass@host:port/db`, `mysql://`, `jdbc:milansql://` DSN support |
| **80** | **Column Store Engine (OLAP)** — `CREATE COLUMN TABLE`, columnar SUM/AVG/COUNT/MIN/MAX, `SHOW STORAGE FORMAT` |
| **81** | **Logical Replication** — `CREATE PUBLICATION … FOR TABLE`, `CREATE SUBSCRIPTION … CONNECTION`, `SHOW LOGICAL LOG` |
| **82** | **Adaptive Query Optimizer** — Query rewriting (WHERE 1=1, redundant predicates), adaptive index suggestions, `EXPLAIN REWRITTEN`, `SHOW QUERY STATS` |

---

## Feature Overview

| Category | Features |
|----------|---------|
| **SQL** | SELECT/INSERT/UPDATE/DELETE, INNER/LEFT/RIGHT/FULL OUTER JOIN, correlated Subqueries, CTEs (`WITH`), Window Functions, `CASE WHEN`, `UNION`/`INTERSECT`/`EXCEPT` |
| **DDL** | `CREATE`/`DROP`/`ALTER TABLE`, Views, Triggers (`BEFORE`/`AFTER`), Stored Procedures with Cursors, B-Tree Indexes, Full-Text Search |
| **Constraints** | `PRIMARY KEY`, `FOREIGN KEY` (CASCADE/SET NULL/RESTRICT), `NOT NULL`, `UNIQUE`, `DEFAULT`, `CHECK`, `AUTO_INCREMENT` |
| **Transactions** | `BEGIN`/`COMMIT`/`ROLLBACK`, `SAVEPOINT`, WAL-based crash recovery, `SELECT FOR UPDATE`, `LOCK TABLE READ\|WRITE`, MVCC (xmin/xmax), `VACUUM`, `SET TRANSACTION ISOLATION LEVEL`, `SHOW TRANSACTIONS`, `SHOW RECOVERY STATUS` |
| **Materialized Views** | `CREATE MATERIALIZED VIEW AS SELECT ...`, `SELECT` (cached), `REFRESH MATERIALIZED VIEW`, `DROP MATERIALIZED VIEW`, `SHOW MATERIALIZED VIEWS`, persisted in `database.matviews` |
| **Buffer Pool** | LRU page manager, `SHOW BUFFER POOL STATUS` (hit rate, dirty pages, evictions), `SET BUFFER_POOL_SIZE = N`, `FLUSH BUFFER POOL`, Write-Behind background thread |
| **MySQL Protocol** | MySQL Wire Protocol v10 (`--mysql --mysql-port 4407`), COM_QUERY/QUIT/PING/INIT_DB, resultset format, `mysql` CLI compatible |
| **Data Types** | `INT`, `TEXT`, `REAL`, `DATE`, `TIME`, `DATETIME`, `TIMESTAMP`, `JSON` |
| **Functions** | String (`UPPER`/`LOWER`/`CONCAT`/`SUBSTR`/`REPLACE`/`TRIM`), Math (`ABS`/`ROUND`/`SQRT`/`POWER`), Date (`NOW`/`DATEDIFF`/`DATE_ADD`/`DATE_FORMAT`), JSON (`JSON_EXTRACT`/`JSON_SET`/`JSON_KEYS`), Regex (`REGEXP_REPLACE`/`REGEXP_EXTRACT`), Null (`COALESCE`/`IFNULL`), Window (`ROW_NUMBER`/`RANK`/`DENSE_RANK`) |
| **Server** | TCP/IP multi-threaded server (port 4406), REST API (port 8080), MySQL Wire Protocol (port 4407), connection pool, multi-statement queries |
| **Replication** | Master/Slave, Binlog, auto-sync every 500ms, auto-reconnect, read-only slave |
| **Admin** | `BACKUP`/`RESTORE`, CSV import/export, Event Scheduler, Partitioning (RANGE/LIST/HASH), `INFORMATION_SCHEMA`, User Management (`GRANT`/`REVOKE`) |
| **Performance** | Cost-based query optimizer, B-Tree index lookup, `EXPLAIN`/`EXPLAIN ANALYZE`, Query Cache (LRU/TTL), `BENCHMARK` command, Query Profiler (`PROFILE ON/OFF`, `SHOW PROFILES`) |
| **Spatial** | `POINT(lat,lng)` type, `ST_DISTANCE` (Haversine), `ST_X`/`ST_Y`/`ST_WITHIN`/`ST_ASTEXT`, `CREATE SPATIAL INDEX` |
| **Row-Level Security** | `ENABLE/DISABLE ROW LEVEL SECURITY`, `CREATE/DROP POLICY`, `SHOW POLICIES ON`, `CURRENT_USER_ID()` in expressions |
| **Pub/Sub** | `LISTEN`/`UNLISTEN`/`NOTIFY` channel pub/sub, payload support, `SHOW LISTEN` |
| **Parallel Queries** | `SET PARALLEL_THRESHOLD`, `SET MAX_PARALLEL_WORKERS`, `/*+ PARALLEL(N) */` hint, `SHOW PARALLEL STATUS` |
| **Table Inheritance** | `CREATE TABLE child INHERITS (parent)`, inherited SELECT, `SELECT FROM ONLY`, `SHOW INHERITANCE` |
| **Column Store** | `CREATE COLUMN TABLE` (OLAP), columnar aggregation (SUM/AVG/MIN/MAX/COUNT), `SHOW STORAGE FORMAT` |
| **Logical Replication** | `CREATE/DROP PUBLICATION FOR TABLE`, `CREATE/DROP SUBSCRIPTION CONNECTION`, `ALTER SUBSCRIPTION ENABLE/DISABLE`, `SHOW LOGICAL LOG` |
| **Adaptive Optimizer** | Query rewriting (1=1 removal, redundant predicates, subquery notes), `SHOW QUERY STATS`, `SHOW INDEX SUGGESTIONS`, `EXPLAIN REWRITTEN`, `ANALYZE TABLE` |
| **Connection Strings** | `milansql://user:pass@host:port/db`, `mysql://`, `jdbc:milansql://` DSN support |

---

## Quick Start

```bash
# Clone & build (Linux/macOS)
git clone https://github.com/haidari9819-lang/milansql
cd milansql
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/milansql
```

```bash
# Windows (MSYS2 UCRT64)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/milansql.exe
```

---

## REPL

```
  ╔══════════════════════════════════════════╗
  ║        === MilanSQL v3.0.0 ===           ║
  ║   Built with <3 by Mirwais Haidari       ║
  ║  Type 'help' for commands, 'exit' to quit║
  ╚══════════════════════════════════════════╝

milansql> CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name TEXT, score INT)
  Tabelle 'users' erstellt.

milansql> INSERT INTO users VALUES (NULL, Alice, 95); INSERT INTO users VALUES (NULL, Bob, 82); SELECT * FROM users;
  1 Zeile(n) eingefuegt.
  1 Zeile(n) eingefuegt.
  ┌────┬───────┬───────┐
  │ id │ name  │ score │
  ├────┼───────┼───────┤
  │ 1  │ Alice │ 95    │
  │ 2  │ Bob   │ 82    │
  └────┴───────┴───────┘
  2 Zeile(n).
```

---

## Server Modes

### TCP Server

```bash
./build/milansql --server --port 4406 --pool-size 20
./build/milansql --client --port 4406   # connect from another terminal
```

### REST API

```bash
./build/milansql --http --port 8080
```

```bash
# Single statement
curl -s -X POST http://localhost:8080/query \
  -H 'Content-Type: application/json' \
  -d '{"sql":"SELECT * FROM users"}'

# Multi-statement (Phase 67)
curl -s -X POST http://localhost:8080/query \
  -H 'Content-Type: application/json' \
  -d '{"sql":"INSERT INTO users VALUES (NULL, Alice, 95); INSERT INTO users VALUES (NULL, Bob, 82); SELECT * FROM users;"}'
```

**Single-statement response:**
```json
{
  "success": true,
  "columns": ["id", "name", "score"],
  "rows": [[1, "Alice", 95], [2, "Bob", 82]],
  "rowCount": 2
}
```

**Multi-statement response:**
```json
{
  "success": true,
  "count": 3,
  "results": [
    {"statement": "INSERT ...", "result": {"success": true, "rowsAffected": 1}},
    {"statement": "INSERT ...", "result": {"success": true, "rowsAffected": 1}},
    {"statement": "SELECT ...", "result": {"success": true, "columns": [...], "rows": [...]}}
  ]
}
```

### MySQL Wire Protocol (Phase 74)

```bash
./build/milansql --mysql --mysql-port 4407

# Connect with standard MySQL CLI:
mysql -h 127.0.0.1 -P 4407 -u root --skip-ssl

# Or Python mysql-connector:
# pip install mysql-connector-python
# import mysql.connector
# conn = mysql.connector.connect(host='localhost', port=4407, user='root')
```

### Master/Slave Replication

```bash
./build/milansql --server --master --repl-port 4408
./build/milansql --server --slave --master-host localhost --master-port 4408
```

---

## Client Libraries

### Python (DB-API 2.0 / PEP 249)

```bash
pip install -e clients/python
```

```python
import milansql

with milansql.connect(host='localhost', port=4406) as conn:
    cur = conn.cursor()
    cur.execute("INSERT INTO users VALUES (NULL, %s, %s)", ("Alice", 95))
    cur.execute("SELECT * FROM users WHERE score > %s", (80,))
    for row in cur.fetchall():
        print(row)  # (1, 'Alice', 95)
```

### Node.js

```js
const milansql = require('./clients/nodejs');
const conn = milansql.connect({ host: 'localhost', port: 8080 });

const result = await conn.query('SELECT * FROM users ORDER BY score DESC');
console.log(result.columns);  // ['id', 'name', 'score']
console.log(result.rows);     // [[1, 'Alice', 95], ...]
```

---

## SQL Reference

```sql
-- Generated Columns (Phase 68)
CREATE TABLE produkte (
  id     INT  PRIMARY KEY AUTO_INCREMENT,
  name   TEXT NOT NULL,
  netto  REAL,
  brutto REAL GENERATED ALWAYS AS (netto * 1.19) STORED,
  mwst   REAL GENERATED ALWAYS AS (netto * 0.19) VIRTUAL
);
INSERT INTO produkte VALUES (NULL, 'Laptop', 1000);
SELECT * FROM produkte;
-- brutto: 1190  |  mwst: 190  (auto-computed)

-- WAL Crash Recovery (Phase 72)
SHOW RECOVERY STATUS;
-- WAL found: yes | Recovered TX: 2 | Discarded TX: 1 | Replayed ops: 5

-- Materialized Views (Phase 72)
CREATE MATERIALIZED VIEW mv_stats AS SELECT dept, COUNT(*) FROM emp GROUP BY dept;
SELECT * FROM mv_stats;               -- uses cached data
REFRESH MATERIALIZED VIEW mv_stats;   -- re-execute query, update cache
SHOW MATERIALIZED VIEWS;
DROP MATERIALIZED VIEW mv_stats;

-- MVCC (Phase 71)
SET TRANSACTION ISOLATION LEVEL REPEATABLE READ;
BEGIN;
SELECT * FROM konto;          -- snapshot from BEGIN
UPDATE konto SET balance = 9999 WHERE name = Alice;
SELECT * FROM konto;          -- still sees old values (REPEATABLE READ)
COMMIT;
SELECT * FROM konto;          -- now sees updated values
BEGIN; DELETE FROM konto WHERE name = Bob; COMMIT;
VACUUM;                       -- physically remove logically deleted rows
SHOW TRANSACTIONS;            -- list active transactions

-- Spatial (Phase 70)
CREATE TABLE staedte (id INT PRIMARY KEY AUTO_INCREMENT, name TEXT, pos TEXT);
INSERT INTO staedte VALUES (NULL, 'Berlin', 'POINT(52.52, 13.40)');
INSERT INTO staedte VALUES (NULL, 'Hamburg', 'POINT(53.55, 10.00)');
SELECT name, ST_DISTANCE(pos, 'POINT(52.52, 13.40)') FROM staedte;
SELECT * FROM staedte WHERE ST_DISTANCE(pos, 'POINT(52.52, 13.40)') < 300;
CREATE SPATIAL INDEX idx_pos ON staedte (pos);

-- Query Profiler (Phase 69)
PROFILE ON;
SELECT * FROM staedte WHERE ST_X(pos) > 50;
SHOW PROFILES;
SHOW PROFILE FOR QUERY 1;
PROFILE OFF;

-- Multi-Statement (Phase 67)
CREATE TABLE t (id INT PRIMARY KEY AUTO_INCREMENT, name TEXT);
INSERT INTO t VALUES (NULL, Alice); INSERT INTO t VALUES (NULL, Bob); SELECT * FROM t;

-- Transactions & Locking
BEGIN;
  SELECT * FROM orders WHERE id = 1 FOR UPDATE;
  UPDATE orders SET status = 'processed' WHERE id = 1;
COMMIT;

LOCK TABLE inventory WRITE;
UPDATE inventory SET qty = qty - 1 WHERE sku = 'X100';
UNLOCK TABLES;

SAVEPOINT sp1;
INSERT INTO log VALUES (NULL, 'step1');
ROLLBACK TO SAVEPOINT sp1;

-- Stored Procedure with Cursor (Phase 66)
CREATE PROCEDURE process_kids(p_age INT)
BEGIN
  DECLARE done INT DEFAULT 0;
  DECLARE kid INT;
  DECLARE cur CURSOR FOR SELECT id FROM kinder WHERE age > p_age;
  DECLARE CONTINUE HANDLER FOR NOT FOUND SET done = 1;
  OPEN cur;
  myloop: LOOP
    FETCH cur INTO kid;
    IF done = 1 THEN LEAVE myloop END IF;
    INSERT INTO result_log VALUES (NULL, kid);
  END LOOP;
  CLOSE cur;
END
CALL process_kids(10)

-- Window Functions
SELECT name, score,
  ROW_NUMBER() OVER (PARTITION BY dept ORDER BY score DESC) AS rank
FROM employees;

-- CTE
WITH top_sales AS (
  SELECT seller_id, SUM(amount) AS total FROM sales GROUP BY seller_id
)
SELECT s.name, t.total FROM sellers s INNER JOIN top_sales t ON s.id = t.seller_id
ORDER BY t.total DESC LIMIT 10;

-- JSON
SELECT JSON_EXTRACT(meta, '$.city') AS city FROM users WHERE JSON_EXTRACT(meta, '$.age') > 25;

-- Regex
SELECT * FROM products WHERE name REGEXP '^[A-Z][a-z]+';
SELECT REGEXP_REPLACE(description, '\s+', ' ') AS clean FROM articles;

-- Partitioning
CREATE TABLE sales (id INT, amount INT, region TEXT)
  PARTITION BY LIST (region) (
    PARTITION p_eu VALUES IN ('DE', 'FR', 'AT'),
    PARTITION p_us VALUES IN ('NY', 'CA', 'TX')
  );
SHOW PARTITIONS FROM sales;

-- INFORMATION_SCHEMA
SELECT TABLE_NAME, TABLE_ROWS FROM information_schema.tables;
SELECT COLUMN_NAME, DATA_TYPE, IS_NULLABLE FROM information_schema.columns WHERE TABLE_NAME = 'users';

-- Backup / Restore
BACKUP DATABASE TO 'backup_2026.sql';
RESTORE DATABASE FROM 'backup_2026.sql';

-- CSV
LOAD DATA INFILE 'data.csv' INTO TABLE users SEPARATOR ',' SKIP HEADER;
SELECT * FROM users INTO OUTFILE 'export.csv' SEPARATOR ',';

-- Event Scheduler
CREATE EVENT cleanup ON SCHEDULE EVERY 1 DAY DO DELETE FROM log WHERE created < DATE_SUB(NOW(), INTERVAL 30 DAY);
SET EVENT_SCHEDULER = ON;

-- Row-Level Security (Phase 75)
ENABLE ROW LEVEL SECURITY ON orders;
CREATE POLICY user_orders ON orders FOR SELECT TO alice USING (user_id = CURRENT_USER_ID());
SHOW POLICIES ON orders;

-- LISTEN / NOTIFY (Phase 76)
LISTEN updates;
NOTIFY updates, 'new row inserted';
SHOW LISTEN;

-- Table Inheritance (Phase 78)
CREATE TABLE fahrzeug (id INT PRIMARY KEY AUTO_INCREMENT, marke TEXT);
CREATE TABLE pkw (tueren INT) INHERITS (fahrzeug);
SELECT * FROM fahrzeug;   -- includes rows from pkw
SELECT * FROM ONLY fahrzeug;
SHOW INHERITANCE;

-- Column Store (OLAP) (Phase 80)
CREATE COLUMN TABLE sales (region TEXT, umsatz INT, quartal INT);
INSERT INTO sales VALUES (DE, 50000, 1);
SELECT SUM(umsatz) FROM sales;
SELECT AVG(umsatz) FROM sales WHERE quartal = 1;
SHOW STORAGE FORMAT;

-- Logical Replication (Phase 81)
CREATE PUBLICATION pub_orders FOR TABLE orders;
CREATE SUBSCRIPTION sub1 CONNECTION 'milansql://localhost:4406' PUBLICATION pub_orders;
SHOW PUBLICATIONS;
SHOW LOGICAL LOG;

-- Adaptive Query Optimizer (Phase 82)
SET QUERY_REWRITE = ON;
SELECT COUNT(*) FROM users WHERE 1=1;   -- WHERE 1=1 auto-removed
SHOW QUERY STATS;
SHOW INDEX SUGGESTIONS;
ANALYZE TABLE users;
EXPLAIN REWRITTEN SELECT * FROM orders WHERE user_id IN (SELECT id FROM users WHERE active = 1);

-- Connection String (Phase 79)
./build/milansql milansql://alice:secret@localhost:4406/mydb
```

---

## Project Structure

```
milansql/
├── CMakeLists.txt
├── .github/workflows/ci.yml       # GitHub Actions (Ubuntu + Windows)
├── src/
│   ├── main.cpp                   # REPL + CLI argument parsing
│   ├── dispatch.hpp               # SQL command dispatcher + splitStatements()
│   ├── engine/
│   │   ├── engine.hpp             # Core engine: tables, constraints, joins,
│   │   │                          # transactions (WAL), views, triggers,
│   │   │                          # procedures, window functions, partitions,
│   │   │                          # full-text search, schemas, locking
│   │   └── btree.hpp              # B-Tree index (in-memory, T=3)
│   ├── parser/
│   │   └── parser.hpp             # SQL tokenizer & parser → ParsedCommand
│   ├── storage/
│   │   └── storage.hpp            # Binary format v7 (XOR checksum)
│   ├── optimizer/
│   │   ├── optimizer.hpp          # Cost-based query optimizer
│   │   ├── query_rewriter.hpp     # Phase 82: rule-based query rewriting
│   │   └── adaptive_stats.hpp     # Phase 82: adaptive index suggestions
│   ├── locking/
│   │   └── lock_manager.hpp       # Row-level + table-level locking
│   ├── replication/
│   │   ├── binlog.hpp             # Binary log writer/reader
│   │   ├── master_repl.hpp        # Master replication server
│   │   ├── slave_repl.hpp         # Slave polling client
│   │   ├── repl_state.hpp         # Global replication state
│   │   └── logical_repl.hpp       # Phase 81: logical replication log
│   ├── buffer/
│   │   └── buffer_pool.hpp        # Buffer Pool Manager (LRU + write-behind)
│   ├── wal/
│   │   └── wal_recovery.hpp       # WAL crash recovery (scan + replay)
│   ├── scheduler/
│   │   └── event_scheduler.hpp    # Event scheduler (CREATE EVENT)
│   ├── backup/
│   │   └── backup.hpp             # SQL dump backup/restore
│   ├── utils/
│   │   ├── csv_utils.hpp          # CSV import/export (RFC-4180)
│   │   └── connection_string.hpp  # Phase 79: DSN parser (milansql:// etc.)
│   ├── storage/
│   │   ├── storage.hpp            # Binary format v9
│   │   └── column_store.hpp       # Phase 80: columnar storage (OLAP)
│   ├── pubsub/
│   │   └── pubsub.hpp             # Phase 76: LISTEN/NOTIFY/UNLISTEN
│   ├── parallel/
│   │   └── parallel_executor.hpp  # Phase 77: parallel query config
│   ├── server/
│   │   ├── server.hpp             # TCP server (Winsock2/POSIX, thread pool)
│   │   ├── client.hpp             # TCP client REPL
│   │   ├── http_server.hpp        # HTTP/JSON REST API server
│   │   ├── mysql_server.hpp       # MySQL Wire Protocol v10 server (Phase 74)
│   │   └── pool_stats.hpp         # Connection pool statistics
│   └── tests/
│       ├── milansql_tests.cpp     # 41 automated tests
│       └── benchmark.cpp          # Performance benchmark
└── clients/
    ├── python/                    # Python package (DB-API 2.0 / PEP 249)
    └── nodejs/                    # Node.js client (HTTP, 0 dependencies)
```

---

## Build & Test

### Prerequisites

- **CMake** ≥ 3.16
- **C++17** compiler (GCC ≥ 7, Clang ≥ 5, MSVC 2017+)
- Windows: [MSYS2](https://www.msys2.org/) UCRT64 recommended

```bash
# Build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Run tests (41 tests)
./build/milansql_tests

# Benchmark
./build/milansql_bench
```

| Benchmark | Time |
|-----------|------|
| 10,000 INSERTs | ~120ms |
| Full scan SELECT | ~2ms |
| B-Tree index SELECT | <1ms |
| 10,000 indexed SELECTs | ~8ms |

GitHub Actions runs build + tests automatically on **Ubuntu** and **Windows** on every push to `main`.

---

## Development Phases (v1.0 → v2.0)

| Phases | Milestone |
|--------|-----------|
| 1–12 | SQL core: DDL/DML/SELECT, WHERE, ORDER BY, LIMIT, B-Tree index, binary storage |
| 13–16 | JOINs (INNER/LEFT/RIGHT/FULL), GROUP BY, Subqueries, NULL support |
| 17–23 | Transactions (WAL), Constraints (PK/FK/UNIQUE/DEFAULT/CHECK/AUTO_INCREMENT) |
| 24–36 | Views, SHOW TABLES, Multi-row INSERT, UNION/INTERSECT/EXCEPT, CASE WHEN, String/Math functions |
| 37–42 | Correlated subqueries, UPSERT, CAST, CTE, Window Functions |
| 43–46 | Triggers, Stored Procedures, Prepared Statements, User Management (GRANT/REVOKE) |
| 47–53 | TCP Server, Cost-Based Optimizer, Full-Text Search, Schemas, REST API, Client Libraries |
| 54–58 | Query Cache, EXPLAIN ANALYZE, Web Dashboard, DATE/TIME/JSON types, Backup/Restore, Connection Pool |
| 59–61 | Master/Slave Replication (Binlog), CSV Import/Export, Event Scheduler |
| 62–63 | Partitioning (RANGE/LIST/HASH), INFORMATION_SCHEMA |
| 64–65 | REGEXP/RLIKE, SAVEPOINT, SELECT FOR UPDATE, LOCK TABLE |
| **66–67** | **Cursors in Stored Procedures (DECLARE/OPEN/FETCH/CLOSE/LOOP/IF), Multi-Statement Queries** |
| **68** | **Virtual/Generated Columns (GENERATED ALWAYS AS (expr) STORED/VIRTUAL)** |
| **69** | **Query Profiler (PROFILE ON/OFF, SHOW PROFILES, SHOW PROFILE FOR QUERY n)** |
| **70** | **Spatial Index (POINT type, ST_DISTANCE/Haversine, ST_X/ST_Y/ST_WITHIN/ST_ASTEXT, CREATE SPATIAL INDEX)** |
| **71** | **MVCC (xmin/xmax versioned rows, TransactionManager, VACUUM, SET TRANSACTION ISOLATION LEVEL, SHOW TRANSACTIONS)** |
| **72** | **WAL Crash Recovery (TX_BEGIN/TX_COMMIT/TX_ROLLBACK markers, replay on startup, SHOW RECOVERY STATUS) + Materialized Views (CREATE/REFRESH/DROP/SHOW, persistence in database.matviews)** |
| **73** | **Buffer Pool Manager (LRU eviction, dirty page tracking, hit/miss counters, SHOW BUFFER POOL STATUS, SET BUFFER_POOL_SIZE, FLUSH BUFFER POOL, Write-Behind thread)** |
| **74** | **MySQL Wire Protocol v10 (COM_QUERY/QUIT/PING/INIT_DB, handshake, resultset, `--mysql --mysql-port 4407`, compatible with `mysql` CLI and Python mysql-connector)** |
| **75** | **Row-Level Security (ENABLE/DISABLE RLS, CREATE/DROP POLICY, SHOW POLICIES ON, CURRENT_USER_ID() in USING expressions, policy persistence in database.rls)** |
| **76** | **LISTEN/NOTIFY/UNLISTEN Pub/Sub (channel subscriptions, payload support, cross-session delivery, SHOW LISTEN, thread-safe PubSub global)** |
| **77** | **Parallel Query Execution (std::thread-based parallelFilter/parallelAggregate, SET PARALLEL_THRESHOLD/MAX_PARALLEL_WORKERS, `/*+ PARALLEL(N) */` hint)** |
| **78** | **Table Inheritance PostgreSQL-style (CREATE TABLE child INHERITS (parent), inherited rows in SELECT, SELECT FROM ONLY, SHOW INHERITANCE, binary format v9)** |
| **79** | **Connection String Parser (milansql://user:pass@host:port/db, mysql://, jdbc:milansql://, auto-CONNECT+USE on startup, Python/Node.js DSN support)** |
| **80** | **Column Store Engine OLAP (CREATE COLUMN TABLE, columnar SUM/AVG/COUNT/MIN/MAX, GROUP BY fallback via temp table, SHOW STORAGE FORMAT)** |
| **81** | **Logical Replication Publication/Subscription (CREATE PUBLICATION FOR TABLE/ALL TABLES, CREATE SUBSCRIPTION CONNECTION, ALTER SUBSCRIPTION ENABLE/DISABLE, SHOW LOGICAL LOG, append-only log)** |
| **82** | **Adaptive Query Optimizer + Query Rewriting (WHERE 1=1 removal, redundant predicate elimination, subquery-to-JOIN notes, SHOW QUERY STATS, SHOW INDEX SUGGESTIONS, ANALYZE TABLE, EXPLAIN REWRITTEN, database.stats persistence)** |

---

## License

MIT License — free for learning, research, and demo purposes.

*MilanSQL — A relational database engine in C++17, built from scratch by Mirwais Haidari.*
