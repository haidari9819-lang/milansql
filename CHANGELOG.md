# Changelog

All notable changes to MilanSQL are documented in this file.

---

## [v3.0.0] — 2026-05-30

### Major Release — 82 Phases, Column Store, Logical Replication, Adaptive Optimizer

Version 3.0.0 marks a major milestone: 82 development phases, zero external dependencies, and a new generation of analytical and optimization capabilities.

**New capabilities:**
- Columnar storage for OLAP workloads (Column Store Engine)
- Logical publication/subscription replication with append-only change log
- Adaptive query optimizer with automatic index suggestions and query rewriting
- Connection string (DSN) parser with milansql://, mysql://, and jdbc:milansql:// protocols
- Table Inheritance (PostgreSQL-style) with inherited SELECTs and `FROM ONLY`
- Parallel query execution with thread-based aggregation
- Row-Level Security with policy expressions and CURRENT_USER_ID()
- LISTEN/NOTIFY/UNLISTEN pub/sub messaging

---

### Phase 82 — Adaptive Query Optimizer + Query Rewriting

**Features:**
- `src/optimizer/query_rewriter.hpp`: `QueryRewriter` class — rule-based rewrites before execution
  - **B) WHERE 1=1 removal** — always-true conditions stripped automatically
  - **C) Redundant predicate elimination** — `WHERE col > 100 AND col > 50` → `WHERE col > 100`
  - **A) Subquery-to-JOIN notes** — `IN (SELECT …)` flagged as JOIN candidate (inList resolution active)
- `src/optimizer/adaptive_stats.hpp`: `AdaptiveStats` class — tracks table/column access patterns
  - `recordQuery(ParsedCommand)` — counts per-table accesses and per-column WHERE frequency
  - Index suggestions triggered at ≥20% query frequency for a column
  - Persistent: `database.stats` (loaded on startup, saved on ANALYZE/exit)
- New commands:
  - `SET QUERY_REWRITE = ON|OFF` — enable/disable automatic rewriting
  - `SHOW QUERY STATS` — table with access counts and hottest WHERE column per table
  - `SHOW INDEX SUGGESTIONS` — `CREATE INDEX …` statements for high-frequency filter columns
  - `ANALYZE TABLE name` — reset statistics for a table (fresh baseline)
  - `EXPLAIN REWRITTEN SELECT …` — show original query, applied rewrites, and execution plan
- `g_queryRewriter` + `g_adaptiveStats` globals in `dispatch.hpp`; integrated before every dispatch

**Example:**
```sql
SET QUERY_REWRITE = ON;
SELECT COUNT(*) FROM users WHERE 1=1;              -- WHERE 1=1 silently removed
SELECT * FROM users WHERE active = 1;
SELECT * FROM orders WHERE user_id = 1;
SHOW QUERY STATS;
SHOW INDEX SUGGESTIONS;
-- CREATE INDEX idx_orders_user_id ON orders(user_id)  -- 80% der Queries filtern nach user_id
EXPLAIN REWRITTEN SELECT * FROM orders WHERE user_id IN (SELECT id FROM users WHERE active = 1);
```

**Technical changes:**
- `src/optimizer/query_rewriter.hpp`: new file — `QueryRewriter` class; uses `WhereCondition.col`/`.val`
- `src/optimizer/adaptive_stats.hpp`: new file — `AdaptiveStats` class with load/save ctor/dtor
- `parser.hpp`: 5 new `CommandType` values; `queryRewriteFlag` field; `EXPLAIN REWRITTEN` parsed before generic EXPLAIN strip; `SET QUERY_REWRITE` and `ANALYZE TABLE` branches; SHOW QUERY STATS / SHOW INDEX SUGGESTIONS in SHOW block
- `dispatch.hpp`: includes for new headers; `g_queryRewriter` + `g_adaptiveStats` statics; `recordQuery` + `rewrite` called at top of `dispatchCommand`; 5 new cases
- `main.cpp`: v3.0.0 banner
- `CMakeLists.txt`: VERSION 3.0.0

---

### Phase 81 — Logical Replication (Publication/Subscription)

**Features:**
- `src/replication/logical_repl.hpp`: `LogChange` struct + `LogicalReplLog` class (append-only log at `database.logical.log`)
- `CREATE PUBLICATION name FOR TABLE t1, t2` / `FOR ALL TABLES`
- `DROP PUBLICATION name` / `SHOW PUBLICATIONS`
- `CREATE SUBSCRIPTION name CONNECTION 'dsn' PUBLICATION pub`
- `DROP SUBSCRIPTION name` / `SHOW SUBSCRIPTIONS`
- `ALTER SUBSCRIPTION name ENABLE|DISABLE`
- `SHOW LOGICAL LOG` — shows all entries from log file
- `insertRow` hooks into `logicalLog_WriteIfPublished_` after every successful insert
- Persistence: `database.publications` + `database.subscriptions`

**Example:**
```sql
CREATE PUBLICATION pub_orders FOR TABLE orders;
SHOW PUBLICATIONS;
INSERT INTO orders VALUES (NULL, 1, 500);  -- automatically logged
SHOW LOGICAL LOG;
CREATE SUBSCRIPTION sub1 CONNECTION 'milansql://localhost:4406' PUBLICATION pub_orders;
ALTER SUBSCRIPTION sub1 DISABLE;
DROP SUBSCRIPTION sub1;
DROP PUBLICATION pub_orders;
```

---

### Phase 80 — Column Store Engine (OLAP Analytics)

**Features:**
- `src/storage/column_store.hpp`: `ColumnTable` with `map<colName, vector<string>>` columnar storage
- `CREATE COLUMN TABLE name (cols…)` — separate from row-store tables
- Fast aggregation: `SELECT SUM/AVG/COUNT/MIN/MAX(col) FROM col_table` → single column scan
- WHERE filters on column tables supported
- GROUP BY falls back to row conversion via temp table `__cs_tmp__`
- `SHOW STORAGE FORMAT` — lists all tables with ROW STORE or COLUMN STORE label

**Example:**
```sql
CREATE COLUMN TABLE sales (region TEXT, umsatz INT, quartal INT);
INSERT INTO sales VALUES (DE, 50000, 1);
INSERT INTO sales VALUES (AT, 30000, 1);
SELECT SUM(umsatz) FROM sales;
SELECT AVG(umsatz) FROM sales WHERE quartal = 1;
SHOW STORAGE FORMAT;
```

---

### Phase 79 — Connection String Parser

**Features:**
- `src/utils/connection_string.hpp`: `ConnectionString` struct with `parse()`, `isDsn()`, `validate()`, `toDisplayString()`
- Protocols: `milansql://`, `mysql://`, `jdbc:milansql://`; defaults: host=localhost, port=4406, db=public, user=root
- `main.cpp`: detect DSN CLI arg, auto-execute CONNECT + USE before REPL
- Python client: `connect()` accepts DSN string + `parse_dsn(dsn)` function
- Node.js client: `parseDsn()` + `connect()` accepts string DSN

**Example:**
```bash
./build/milansql milansql://alice:secret@localhost:4406/mydb
./build/milansql mysql://root@localhost:4407/public
```

---

### Phase 78 — Table Inheritance (PostgreSQL-style)

**Features:**
- `CREATE TABLE child (col TYPE) INHERITS (parent)` — child prepends parent columns
- `SELECT * FROM parent` — includes rows from all descendants
- `SELECT * FROM ONLY parent` — only parent rows
- `SELECT COUNT(*) FROM parent` — counts all descendants recursively
- `SHOW INHERITANCE` — prints tree of all inheritance relationships
- Binary format v9: stores `parentName` per table

**Example:**
```sql
CREATE TABLE fahrzeug (id INT PRIMARY KEY AUTO_INCREMENT, marke TEXT);
CREATE TABLE pkw (tueren INT) INHERITS (fahrzeug);
INSERT INTO pkw VALUES (NULL, BMW, 4);
SELECT * FROM fahrzeug;       -- includes pkw rows
SELECT * FROM ONLY fahrzeug;  -- only direct rows
SHOW INHERITANCE;
```

---

### Phase 77 — Parallel Query Execution

**Features:**
- `src/parallel/parallel_executor.hpp`: `ParallelExecutor` with configurable threshold and worker count
- `parallelFilter_()` splits rows into N chunks using `std::thread`
- `parallelAggregate_()` for COUNT/SUM/MIN/MAX/AVG with partial results merged
- `SET PARALLEL_THRESHOLD = N` / `SET MAX_PARALLEL_WORKERS = N` / `SHOW PARALLEL STATUS`
- `/*+ PARALLEL(N) */` hint in SELECT overrides worker count for that query

---

### Phase 76 — LISTEN/NOTIFY/UNLISTEN Pub/Sub

**Features:**
- `src/pubsub/pubsub.hpp`: thread-safe `PubSub` class + `g_pubsub()` global singleton
- `LISTEN channel` — subscribe current user to channel
- `UNLISTEN channel` / `UNLISTEN *` — unsubscribe
- `NOTIFY channel [, 'payload']` — deliver to all listeners
- `SHOW LISTEN` — list active subscriptions

---

### Phase 75 — Row-Level Security (RLS)

**Features:**
- `ENABLE/DISABLE ROW LEVEL SECURITY` per table
- `CREATE POLICY name ON table FOR cmd TO user USING (expr)`
- `DROP POLICY`, `SHOW POLICIES ON table`
- `CURRENT_USER_ID()` in USING expressions substituted at eval time
- Persisted to `database.rls`; query cache key scoped per user

---

## [v2.5.0] — 2026-05-29

### Phase 73 — Buffer Pool Manager (LRU + Write-Behind + Hit Rate)

**Features:**
- `src/buffer/buffer_pool.hpp`: `BufferPool` class with LRU eviction, dirty page tracking, hit/miss counters, pin/unpin support
- Each "page" = one table; pool size in MB determines max page capacity
- `selectAll()` / `selectWhere()` → `bufferPool_.access()` for hit tracking
- `insertRow()` / `updateWhere()` / `deleteWhere()` → `bufferPool_.markDirty()`
- Background Write-Behind thread (REPL mode): flushes dirty pages to disk every 5 seconds
- `SHOW BUFFER POOL STATUS` — ASCII table with Pool Size, Used/Dirty Pages, Hit Rate, Requests, Hits, Misses, Evictions
- `SET BUFFER_POOL_SIZE = N` — resize pool at runtime (in MB)
- `FLUSH BUFFER POOL` — force flush all dirty pages to disk immediately

**Example:**
```sql
SHOW BUFFER POOL STATUS;
SET BUFFER_POOL_SIZE = 256;
FLUSH BUFFER POOL;
```

**Technical changes:**
- `src/buffer/buffer_pool.hpp`: `PageMeta` struct, `BufferPool` class, `Status` struct, LRU eviction, atomic hit/miss counters
- `engine.hpp`: `mutable BufferPool bufferPool_` member; public API `setBufferPoolSize()`, `showBufferPoolStatus()`, `flushBufferPool()`, `getDirtyBufferPages()`, `markBufferPageClean()`
- `parser.hpp`: `SHOW_BUFFER_POOL_STATUS`, `SET_BUFFER_POOL_SIZE`, `FLUSH_BUFFER_POOL` command types
- `dispatch.hpp`: Phase 73 command handlers
- `main.cpp`: background write-behind thread (every 5s)

---

### Phase 74 — MySQL Wire Protocol (COM_QUERY + Resultset + Handshake)

**Features:**
- `src/server/mysql_server.hpp`: full MySQL Protocol v10 implementation
- Standard MySQL clients connect without any driver changes: `mysql -h 127.0.0.1 -P 4407 -u root --skip-ssl`
- Handshake: sends MySQL v10 greeting with capabilities, charset, auth-plugin-data
- Auth: accepts any password (simulated `mysql_native_password`)
- COM_QUERY (0x03): executes SQL, returns resultset (column defs + rows + EOF) or OK/ERR
- COM_QUIT (0x01): clean disconnect
- COM_PING (0x0e): returns OK
- COM_INIT_DB (0x02): USE database (always OK)
- Special handling for MySQL management queries: `SET NAMES`, `@@version`, `SHOW DATABASES`, `SELECT DATABASE()`, `SELECT USER()`, `SELECT 1`
- Thread-per-connection with engine mutex for concurrent access

**Example:**
```bash
# Start MySQL-compatible server
.\build\milansql.exe --mysql --mysql-port 4407

# Connect with MySQL CLI
mysql -h 127.0.0.1 -P 4407 -u root --skip-ssl

# Or Python socket test
python3 -c "
import socket
s = socket.socket(); s.connect(('localhost', 4407))
greeting = s.recv(4096)
print('Handshake:', len(greeting), 'bytes')
"
```

**Technical changes:**
- `src/server/mysql_server.hpp`: `MysqlServer` class with `run()`, `handleClient()`, `sendHandshake()`, `sendResultSet()`, `sendOK()`, `sendERR()`, `sendEOF()`, packet I/O helpers
- `main.cpp`: `--mysql` and `--mysql-port N` CLI flags; starts `MysqlServer` in dedicated mode

---

## [v2.4.0] — 2026-05-29

### Phase 72 — WAL Crash Recovery + Materialized Views

**TEIL A — WAL Crash Recovery:**
- `src/wal/wal_recovery.hpp`: new `WalRecovery` class with `scanWal()`, `parseOpBlock()`, `recover()`
- Extended WAL format: `TX_BEGIN:<txId>`, `TX_COMMIT:<txId>`, `TX_ROLLBACK:<txId>` markers
- On startup: load binary DB first, then replay all committed-but-not-yet-persisted WAL transactions
- Crash safety: WAL deleted only AFTER successful `persist()` — not at commit time
- `SHOW RECOVERY STATUS` — reports hadWal, recoveredTxCount, discardedTxCount, replayedOpCount

**TEIL B — Materialized Views:**
- `CREATE MATERIALIZED VIEW name AS SELECT ...` — executes query and caches result to memory + disk
- `SELECT * FROM mv_name` — uses cached data (no re-execution), supports WHERE / ORDER BY / LIMIT
- `REFRESH MATERIALIZED VIEW name` — re-executes original SQL, updates cache and `database.matviews`
- `DROP MATERIALIZED VIEW name` — removes from memory and rewrites `database.matviews`
- `SHOW MATERIALIZED VIEWS` — lists all materialized views with name, SQL, and last refresh time
- Persisted in `database.matviews` (binary); re-loaded and refreshed on startup

**Example:**
```sql
-- WAL Recovery (automatic on startup after crash)
SHOW RECOVERY STATUS;
-- WAL found: yes | Recovered TX: 2 | Discarded TX: 1 | Replayed ops: 5

-- Materialized Views
CREATE MATERIALIZED VIEW mv_stats AS SELECT dept, COUNT(*) FROM emp GROUP BY dept;
SELECT * FROM mv_stats;
REFRESH MATERIALIZED VIEW mv_stats;
SHOW MATERIALIZED VIEWS;
DROP MATERIALIZED VIEW mv_stats;
```

**Technical changes:**
- `src/wal/wal_recovery.hpp`: `WalTxEntry` struct, `WalRecovery` class, `RecoveryResult` struct; includes `engine.hpp` after class decl to avoid circular dep
- `engine.hpp`: `beginTransaction()` writes `TX_BEGIN`, `applyAndCommit()` writes `TX_COMMIT` but no longer calls `deleteWal()`, `rollbackTransaction()` writes `TX_ROLLBACK`; added `applyBufferedOp()`, `RecoveryStatus`, `MaterializedViewDef`, full matview CRUD API
- `parser.hpp`: new CommandTypes `SHOW_RECOVERY_STATUS`, `CREATE/REFRESH/DROP_MATERIALIZED_VIEW`, `SHOW_MATERIALIZED_VIEWS`; added `matViewName`/`matViewSql` fields to `ParsedCommand`
- `dispatch.hpp`: COMMIT handler calls `engine.deleteWal()` after `persistFn()`; SELECT checks materialized views before regular views; Phase 72 command handlers
- `main.cpp`: two-step startup (load binary DB → WAL recovery); loads `database.matviews` on startup

---

## [v2.3.0] — 2026-05-29

### Phase 71 — MVCC (Multi-Version Concurrency Control + VACUUM)

**Features:**
- Versioned rows: `xmin` (txId that created the row) and `xmax` (txId that deleted the row, 0 = alive)
- Logical deletes: `DELETE` at COMMIT time stamps `xmax` instead of physically removing rows
- `VACUUM` / `VACUUM ANALYZE` — physically removes all logically deleted rows and rebuilds indexes
- `SHOW TRANSACTIONS` — Unicode table listing all active (uncommitted) transactions with ID, status, start time, isolation level
- `SET TRANSACTION ISOLATION LEVEL READ COMMITTED | REPEATABLE READ | SERIALIZABLE`
- `TransactionManager`: tracks `globalTxId_`, active/committed sets, provides `beginTx`/`commitTx`/`rollbackTx`/`isCommitted`
- WAL-buffer provides natural REPEATABLE READ: SELECTs inside a transaction see pre-BEGIN state until COMMIT
- Query cache invalidated automatically on every COMMIT

**Example:**
```sql
SET TRANSACTION ISOLATION LEVEL REPEATABLE READ;
BEGIN;
SELECT * FROM konto;          -- snapshot from BEGIN
UPDATE konto SET balance = 9999 WHERE name = Alice;
SELECT * FROM konto;          -- still sees old values (REPEATABLE READ)
COMMIT;
SELECT * FROM konto;          -- sees updated values
BEGIN; DELETE FROM konto WHERE name = Bob; COMMIT;
VACUUM;                       -- "1 alte Version(en) bereinigt"
SHOW TRANSACTIONS;            -- "(Keine aktiven Transaktionen)"
```

**Technical changes:**
- New `src/mvcc/transaction_manager.hpp`: `TxInfo` struct + `TransactionManager` class
- `Row` struct: added `uint64_t xmin = 0` and `uint64_t xmax = 0`
- All read paths (`selectWhere`, `computeAggregate`, `groupBy`, `innerJoin`, `executeJoins`, `clone`, `project`, UNIQUE checks, index rebuild) skip rows with `xmax != 0`
- `applyOp`: INSERT stamps `xmin = mvccTxId_`; DELETE stamps `xmax = mvccTxId_` (MVCC logical delete)
- `Storage::save()`: skips rows with `xmax != 0` (no FORMAT_VERSION bump)
- Parser: `VACUUM`, `VACUUM ANALYZE`, `SHOW TRANSACTIONS`, `SET TRANSACTION ISOLATION LEVEL`
- Dispatch: COMMIT handler clears query cache; Phase 71 command handlers added

---

## [v2.2.0] — 2026-05-29

### Phase 70 — Spatial Index (POINT + ST_DISTANCE + Haversine)

**Features:**
- `POINT(lat, lng)` data type stored as canonical string `"POINT(lat lng)"`
- `ST_DISTANCE(p1, p2)` → Haversine great-circle distance in km
- `ST_X(point)` → latitude, `ST_Y(point)` → longitude
- `ST_WITHIN(point, center, radius_km)` → `"1"` or `"0"`
- `ST_ASTEXT(point)` → canonical `"POINT(lat lng)"` string
- `WHERE ST_DISTANCE(col, POINT(x,y)) < n` filter support
- `CREATE SPATIAL INDEX name ON table (col)` — creates index with type `SPATIAL`
- `SHOW INDEXES` now shows `BTREE` vs `SPATIAL` type

**Example:**
```sql
CREATE TABLE staedte (id INT PRIMARY KEY AUTO_INCREMENT, name TEXT, pos TEXT);
INSERT INTO staedte VALUES (NULL, 'Berlin', 'POINT(52.52, 13.40)');
SELECT name, ST_DISTANCE(pos, 'POINT(52.52, 13.40)') FROM staedte;
SELECT * FROM staedte WHERE ST_DISTANCE(pos, 'POINT(52.52, 13.40)') < 300;
CREATE SPATIAL INDEX idx_pos ON staedte (pos);
```

**Technical changes:**
- New `src/spatial/spatial.hpp`: `SpatialUtils` with `parsePoint`, `haversine`, `stDistance`, `stX`, `stY`, `stAsText`, `stWithin`
- `IndexEntry`: added `type` field (`"BTREE"` or `"SPATIAL"`)
- `Table::createIndex()` accepts optional `idxType` parameter
- Parser: `CREATE SPATIAL INDEX`, `ST_*` in FUNC_LHS_NAMES and all ALLFUNCS lists
- Engine: `evaluateFunc` handles ST_* calls; `evalExprStr` KNOWN_FUNCS updated

---

### Phase 69 — Query Profiler (PROFILE ON/OFF + SHOW PROFILES)

**Features:**
- `PROFILE ON` / `PROFILE OFF` — enable/disable query profiling
- `SHOW PROFILES` — table listing all saved profiles (ID, SQL, Duration)
- `SHOW PROFILE FOR QUERY n` — per-step breakdown with durations
- Steps: Optimization, Table scan, Result filtering, Result projection, Sorting
- Profiles stored globally per session (max 100, oldest dropped)

**Example:**
```sql
PROFILE ON;
SELECT * FROM staedte WHERE ST_X(pos) > 50;
SHOW PROFILES;
SHOW PROFILE FOR QUERY 1;
PROFILE OFF;
```

**Technical changes:**
- New `src/profiler/query_profiler.hpp`: `ProfileStep`, `ProfileEntry`, `QueryProfiler` class
- Global `g_profiler` in `dispatch.hpp`; hooks in main SELECT execution path
- Parser: `PROFILE_ON`, `PROFILE_OFF`, `SHOW_PROFILES`, `SHOW_PROFILE_FOR_QUERY` commands

---

## [v2.1.0] — 2026-05-28

### Phase 68 — Virtual/Generated Columns

Implemented MySQL-compatible `GENERATED ALWAYS AS (expr) STORED | VIRTUAL` columns.

**Features:**
- `STORED`: value computed from expression and stored in the row at INSERT/UPDATE
- `VIRTUAL`: same computation, value not persisted (recomputed transparently)
- Arithmetic expression evaluator: column refs, numeric literals, `+`, `-`, `*`, `/`, parentheses
- Auto-recomputed on UPDATE for both STORED and VIRTUAL columns
- `DESCRIBE` output shows `GENERATED` column with `STORED AS (expr)` or `VIRTUAL AS (expr)`
- `BACKUP`/`RESTORE` preserves generated column definitions via SQL dump

**Example:**
```sql
CREATE TABLE produkte (
  id     INT  PRIMARY KEY AUTO_INCREMENT,
  name   TEXT NOT NULL,
  netto  REAL,
  brutto REAL GENERATED ALWAYS AS (netto * 1.19) STORED,
  mwst   REAL GENERATED ALWAYS AS (netto * 0.19) VIRTUAL
);
INSERT INTO produkte VALUES (NULL, 'Laptop', 1000);
SELECT * FROM produkte;  -- brutto: 1190, mwst: 190
```

**Technical changes:**
- `Column` struct: `isGenerated`, `generatedExpr`, `isStored` fields
- `Engine::evaluateGenExpr()` + `ArithEval` recursive descent evaluator
- `Engine::applyGeneratedCols()` called after INSERT/UPDATE
- `FORMAT_VERSION` bumped 7 → 8 (backward-compatible load of v7 files)
- Parser: `GENERATED ALWAYS AS (expr) STORED|VIRTUAL` in `CREATE TABLE`
- `dispatch_printDescribe`: new `GENERATED` column in output table

### Version bumps
- CMakeLists.txt, main.cpp, http_server.hpp → v2.1.0
- README.md, CHANGELOG.md, docs/index.html → v2.1.0 / 68 phases

---

## [v2.0.0] — 2026-05-29

### Major Release — 67 Phases, production-grade SQL engine in C++17

Version 2.0.0 marks the completion of the initial development roadmap. All features from Phase 1 through Phase 67 are fully implemented, tested, and documented.

**Summary of capabilities:**
- Full SQL: SELECT/INSERT/UPDATE/DELETE, all JOIN types, CTEs, Window Functions, Subqueries
- Complete DDL: Tables, Views, Triggers, Stored Procedures (with Cursors), Indexes, Partitioning
- Transactions: WAL-based ACID, SAVEPOINT, SELECT FOR UPDATE, LOCK TABLE
- Data types: INT/TEXT/REAL/DATE/TIME/DATETIME/TIMESTAMP/JSON
- Server: TCP multi-threaded server, REST API, Connection Pool, Multi-Statement Queries
- Replication: Master/Slave Binlog, Auto-Sync, Read-Only Slave
- Admin: Backup/Restore, CSV Import/Export, Event Scheduler, INFORMATION_SCHEMA, User Management
- Performance: Cost-Based Optimizer, B-Tree Indexes, Query Cache, EXPLAIN ANALYZE
- Zero external dependencies — pure C++17

### Version bumps
- CMakeLists.txt, main.cpp, dispatch.hpp, http_server.hpp → v2.0.0
- clients/python/setup.py, clients/nodejs/package.json → 2.0.0
- README fully rewritten for v2.0.0 release

---

## [v1.10.0] — 2026-05-29

### Added

#### Phase 67 — Multi-Statement Queries

- Mehrere `;`-getrennte SQL-Statements in einer einzigen Eingabe ausführen
- REPL: `SELECT 1; SELECT 2; SELECT 3;` → alle 3 Ergebnisse ausgegeben
- HTTP REST API: `POST /query` mit mehreren Statements → kombiniertes JSON-Ergebnis
- `splitStatements(sql)` — teilt Input auf `;` auf, respektiert dabei:
  - `BEGIN...END`-Blöcke (für CREATE TRIGGER / CREATE PROCEDURE)
  - Single-Quoted Strings (`'...'`, inkl. `''`-Escaping)
  - Zeilenkommentare (`-- ...`)
  - Block-Kommentare (`/* ... */`)

**Ergebnis-Format (REST API, mehrere Statements):**
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

### Architecture
- `milansql::splitStatements(input)` — `static inline` in `dispatch.hpp`, nutzbar von REPL und HTTP-Server
- `BEGIN`-depth-Tracking: depth++ bei `BEGIN`, depth-- bei `END` (außer `END IF`/`END LOOP`/`END WHILE`/`END CASE`)
- REPL: Multi-Statement-Zweig vor Parser-Aufruf; einzelnes Statement → unverändertes Verhalten (inkl. UPDATE/DELETE-Bestätigungsdialog)
- HTTP `handleQuery`: `execOne`-Lambda für Einzel-Statement, wrapped in Results-Array bei mehreren Statements; Single-Statement → rückwärtskompatibles Format

---

## [v1.9.0] — 2026-05-28

### Added

#### Phase 66 — Cursor in Stored Procedures (DECLARE/OPEN/FETCH/CLOSE + LOOP/IF)

**Cursor-Unterstützung:**
- `DECLARE name CURSOR FOR SELECT ...` — Cursor deklarieren
- `OPEN name` — Cursor öffnen (führt SELECT aus, lädt Ergebnismenge)
- `FETCH name INTO v1, v2, ...` — nächste Zeile in lokale Variablen laden
- `CLOSE name` — Cursor schließen
- `DECLARE CONTINUE HANDLER FOR NOT FOUND SET var = val` — EOF-Handler

**Variablen:**
- `DECLARE name TYPE [DEFAULT val]` — lokale Variable deklarieren (INT, VARCHAR, etc.)
- `SET var = expr` — Variable setzen (auch arithmetische Ausdrücke: `+`, `-`, `*`, `/`)
- Variable-Substitution: alle Vorkommen lokaler Variablen in SQL-Statements werden substituiert (whole-word boundary)

**Kontrollstrukturen:**
- `label: LOOP ... END LOOP` — Schleife mit Label
- `LEAVE label` — Schleife verlassen
- `IF cond THEN ... [ELSE ...] END IF` — Bedingungsausdruck (verschachtelbar)

### Architecture
- `ProcState` Struct: `vars` (map), `cursors` (map mit CursorSt: sql, rows, colNames, pos, isOpen), `notFoundVar/Val/hasNotFoundHandler`
- `ProcExec` Struct: `execBody()` / `execStmt()` (mutual recursion in same struct), `splitStmts()` (depth-tracking für LOOP/THEN/END LOOP/END IF), `evalCond()`, `execSql()`, `subst()`
- `splitStmts`: Scannt body char-by-char; depth++ bei `LOOP`/`THEN`, depth-- bei `END LOOP`/`END IF`; Split auf `;` nur bei depth==0 — erhält komplette LOOP/IF-Blöcke als einzelne Statements
- CALL_PROCEDURE Handler ersetzt alten `;`-Split durch `ProcExec::execBody()`

---

## [v1.8.0] — 2026-05-28

### Added

#### Phase 65 — SELECT FOR UPDATE + LOCK TABLE (Row/Table Level Locking)

**SELECT FOR UPDATE:**
- `SELECT ... FROM table WHERE ... FOR UPDATE` — sperrt alle Ergebnis-Zeilen mit Row-Level WRITE-Locks
- Locks werden bei `COMMIT` oder `ROLLBACK` automatisch freigegeben
- Wirft Fehler wenn Zeile bereits von einem anderen Thread gesperrt ist

**LOCK TABLE / UNLOCK TABLES:**
- `LOCK TABLE name READ` — exklusiver Read-Lock (blockiert Schreibzugriffe anderer Threads)
- `LOCK TABLE name WRITE` — exklusiver Write-Lock (blockiert alle Zugriffe anderer Threads)
- `UNLOCK TABLES` — gibt alle Tabellen-Locks des aktuellen Threads frei
- `SHOW LOCKS` — zeigt alle aktiven Row- und Table-Locks

**DML-Schutz:**
- `INSERT`, `UPDATE`, `DELETE` prüfen automatisch Tabellen-Locks anderer Threads
- Schreiboperationen auf READ-gesperrte Tabellen → Fehler

### Architecture
- `src/locking/lock_manager.hpp` — `LockManager` Klasse mit `rowLocks_` (map `"table:key"→threadId`) und `tableLocks_` (map `table→{threadId, LockType}`)
- `LockType` Enum (READ, WRITE), `inline LockManager g_lockManager` Global
- `std::mutex` für thread-sichere Lock-Verwaltung; nicht-blockierendes Try-and-Fail-Design
- `Engine::lockTable()`, `unlockTables()`, `showLockInfo()`, `acquireForUpdateLocks()`
- `g_lockManager.releaseRowLocks()` in `applyAndCommit()` und `rollbackTransaction()`
- Write-Guards in `insertRow()`, `updateWhere()`, `updateAll()`, `deleteWhere()` (Phase 65)
- Parser: `isForUpdate` Flag (FOR UPDATE suffix stripping), `lockType` Feld
- `CommandType::LOCK_TABLE`, `UNLOCK_TABLES`, `SHOW_LOCKS`

---

## [v1.7.0] — 2026-05-28

### Added

#### Phase 64 — REGEXP/RLIKE + SAVEPOINT

**REGEXP / RLIKE:**
- `WHERE col REGEXP 'pattern'` — reguläre Ausdrücke in WHERE (C++17 `std::regex`)
- `WHERE col RLIKE 'pattern'` — Alias für REGEXP
- `WHERE col NOT REGEXP 'pattern'` — negierter Regexp-Match
- Unterstützte Features: `^` `$` `.` `*` `+` `?` `[abc]` `[a-z]` `[^abc]` `(a|b)` `\d` `\w` `\s`
- `REGEXP_REPLACE(col, 'pattern', 'repl')` — Regex-Ersetzung in SELECT
- `REGEXP_EXTRACT(col, 'pattern')` — erstes Match extrahieren in SELECT

**SAVEPOINT (verschachtelte Transaktionen):**
- `SAVEPOINT name` — aktuellen Transaktionsstand sichern
- `ROLLBACK TO SAVEPOINT name` — auf gespeicherten Stand zurückrollen (Ops nach dem Savepoint verworfen)
- `ROLLBACK TO name` — Kurzform ohne SAVEPOINT-Keyword
- `RELEASE SAVEPOINT name` — Savepoint entfernen ohne Rollback

### Architecture
- `Engine::compareValues()`: REGEXP/NOT REGEXP via `std::regex_search`
- `Engine::evaluateFunc()`: REGEXP_REPLACE (`std::regex_replace`) + REGEXP_EXTRACT
- `Engine::createSavepoint()` / `rollbackToSavepoint()` / `releaseSavepoint()`
- `Engine::savepointStack_` — vector von `{name, txSize}` (txBuffer_-Größe zum Savepoint-Zeitpunkt)
- `Parser::parseWhere()`: NOT REGEXP / REGEXP / RLIKE erkannt
- Alle 4 Funktionslisten in parser.hpp (SFUNCS, SFUNCS32×2, ALLFUNCS) um REGEXP_REPLACE/REGEXP_EXTRACT erweitert

---

## [v1.6.0] — 2026-05-28

### Added

#### Phase 63 — INFORMATION_SCHEMA
- `SELECT * FROM information_schema.tables` — alle Tabellen und Views (TABLE_SCHEMA, TABLE_NAME, TABLE_TYPE, TABLE_ROWS)
- `SELECT * FROM information_schema.columns` — alle Spalten mit ORDINAL_POSITION, DATA_TYPE, IS_NULLABLE, COLUMN_KEY (PRI/UNI), EXTRA (auto_increment)
- `SELECT * FROM information_schema.indexes` — alle B-Tree-Indizes (TABLE_NAME, INDEX_NAME, COLUMN_NAME, INDEX_TYPE)
- `SELECT * FROM information_schema.views` — alle Views mit VIEW_DEFINITION
- `SELECT * FROM information_schema.triggers` — alle Trigger (EVENT_MANIPULATION, EVENT_OBJECT_TABLE, ACTION_TIMING, ACTION_STATEMENT)
- `SELECT * FROM information_schema.routines` — alle Stored Procedures (ROUTINE_TYPE, ROUTINE_DEFINITION)
- `SELECT * FROM information_schema.schemata` — alle Schemas mit DEFAULT_CHARACTER_SET_NAME
- `SELECT * FROM information_schema.partitions` — partitionierte Tabellen (PARTITION_NAME, PARTITION_METHOD, PARTITION_EXPRESSION, TABLE_ROWS)
- `SELECT * FROM information_schema.events` — alle Events (INTERVAL_VALUE, STATUS, EVENT_DEFINITION)
- `SELECT * FROM information_schema.user_privileges` — Benutzerrechte (GRANTEE, TABLE_NAME, PRIVILEGE_TYPE)
- WHERE-Filter und JOINs auf alle INFORMATION_SCHEMA-Tabellen vollständig unterstützt
- Read-only: INSERT/UPDATE/DELETE auf INFORMATION_SCHEMA → FEHLER
- Virtuelle Tabellen werden on-demand aus dem Engine-State gebaut (keine Persistenz nötig)

### Architecture
- `Engine::isInfoSchemaName(name)` — öffentliche statische Hilfsmethode
- `Engine::buildInfoSchemaTable(rawName)` — baut virtuelle Table-Objekte on-demand
- `Engine::getTable(n) const` — interceptiert `information_schema.*`-Namen via `mutable infoSchemaCache_`
- Write-Protection in `dispatch.hpp` für INSERT/UPDATE/DELETE auf `information_schema.*`

---

## [v1.5.0] — 2026-05-28

### Added

#### Phase 62 — Partitionierung
- `CREATE TABLE … PARTITION BY RANGE (col) (PARTITION p VALUES LESS THAN (n), …)` — RANGE-Partitionierung
- `CREATE TABLE … PARTITION BY LIST (col) (PARTITION p VALUES IN (v1, v2), …)` — LIST-Partitionierung
- `CREATE TABLE … PARTITION BY HASH (col) PARTITIONS n` — HASH-Partitionierung (gleichmäßige Verteilung)
- `SHOW PARTITIONS FROM table` — zeigt Name, Typ, Beschreibung und Zeilenanzahl je Partition
- `ALTER TABLE t ADD PARTITION (PARTITION p VALUES LESS THAN (n))` — RANGE-Partition hinzufügen
- `ALTER TABLE t ADD PARTITION (PARTITION p VALUES IN (v1, v2))` — LIST-Partition hinzufügen
- `ALTER TABLE t DROP PARTITION name` — Partition löschen
- Partition Pruning: `EXPLAIN SELECT * FROM t WHERE col = val` zeigt `PARTITION PRUNING: Partitions: p_klein`
- Persistenz: `database.partitions` (automatisch geladen beim Start)
- Quote-Stripping bei LIST-Wertvergleichen (gespeicherte `'value'` vs. Partitionsdefinition `value`)

### Architecture
- `PartitionType` Enum + `PartitionRangeDef`, `PartitionListDef`, `PartitionInfo` Structs in `engine.hpp`
- `Table::getPartitionName(row)`, `Table::getPartitionStats()`, `Table::prunePartitions(col, op, val)`
- `Engine::showPartitions()`, `Engine::addRangePartition()`, `Engine::addListPartition()`, `Engine::dropPartitionByName()`
- `dispatch_savePartitions()` / `dispatch_loadPartitions()` in `dispatch.hpp`

---

## [v1.4.0] — 2026-05-28

### Added

#### Phase 61 — Event Scheduler
- `CREATE EVENT name ON SCHEDULE EVERY n SECOND|MINUTE|HOUR|DAY|WEEK|MONTH DO sql` — wiederkehrender Job
- `CREATE EVENT name ON SCHEDULE EVERY n DAY AT 'HH:MM:SS' DO sql` — täglich zur fixen Uhrzeit
- `CREATE EVENT name ON SCHEDULE AT 'YYYY-MM-DD HH:MM:SS' DO sql` — einmaliger Job
- `SHOW EVENTS` — alle Events auflisten (Name, Schedule, Status) + Scheduler-Zustand
- `DROP EVENT name` — Event löschen
- `ALTER EVENT name ENABLE|DISABLE` — Event aktivieren/deaktivieren
- `SET EVENT_SCHEDULER = ON|OFF` — Scheduler global starten/stoppen
- Persistenz: `database.events` (Tab-separiert, automatisch geladen beim Start)
- Scheduler-Thread prüft jede Sekunde auf fällige Events; einmalige Events werden nach Ausführung deaktiviert
- Vergangenheitsdaten bei einmaligen Events → automatisch deaktiviert (keine unerwartete Ausführung)

### Architecture
- `src/scheduler/event_scheduler.hpp` — `EventDef` Struct + `EventScheduler` Klasse (Thread-sicher, `g_eventScheduler` Global)

---

## [v1.3.0] — 2026-05-28

### Added

#### Phase 60 — CSV Import/Export
- `LOAD DATA INFILE 'datei.csv' INTO TABLE t [SEPARATOR ','] [SKIP HEADER]` — CSV/TSV-Datei in Tabelle importieren
- `SELECT … INTO OUTFILE 'datei.csv' [SEPARATOR ',']` — Abfrageergebnis als CSV exportieren
- `SHOW DATAFILES` — listet alle `.csv`/`.tsv`-Dateien im Arbeitsverzeichnis mit Dateigröße
- RFC-4180 konformes Parsing (Quoting, `""` Escaping, Auto-Separator-Erkennung)
- Separator-Optionen: `,` `;` `\t` oder beliebiges Zeichen

### Architecture
- `src/utils/csv_utils.hpp` — `CsvUtils` (parseLine, readFile, writeFile, escapeField, detectSeparator, parseSepChar, listCsvFiles)

---

## [v1.2.0] — 2026-05-28

### Added

#### Phase 59 — Master/Slave Replikation
- `--master --repl-port N` — Master-Server mit Replikations-Port
- `--slave --master-host HOST --master-port N` — Slave verbindet sich zum Master
- Binlog (`database.binlog`): jede Schreiboperation wird geloggt (`pos|timestamp|sql`)
- Slave pollt Master alle 500ms, repliziert neue Einträge automatisch
- Auto-Reconnect alle 5s bei Verbindungsunterbrechung
- Slave ist read-only: INSERT/UPDATE/DELETE → Fehler
- `SHOW MASTER STATUS` — Binlog-Position, aktive Slaves
- `SHOW SLAVE STATUS` — Master-Host/Port, Verbindungsstatus, Position, Lag
- `SHOW BINLOG` — letzte 20 Binlog-Einträge
- `STOP SLAVE` / `START SLAVE` — Replikation pausieren/fortsetzen

### Architecture
- `src/replication/binlog.hpp` — `BinlogWriter` (thread-safe Append + Lesen ab Position N)
- `src/replication/repl_state.hpp` — Globaler Zustand + Hooks + `tl_binlogReplay` (thread_local bypass)
- `src/replication/master_repl.hpp` — TCP-Server auf repl-port, bedient Slave-Sync-Anfragen
- `src/replication/slave_repl.hpp` — Polling-Client, replay via `tl_binlogReplay`-Flag

---

## [v1.1.0] — 2026-05-28

### Neue Features

#### Phase 54 — Query Cache + EXPLAIN ANALYZE + Web Dashboard
- LRU/TTL Query Cache: `SET CACHE ON/OFF`, `SHOW CACHE`, `CLEAR CACHE`
- `EXPLAIN ANALYZE` — Ausführungsplan mit echten Laufzeiten
- Web Dashboard unter `--http --port 8080` (HTML5 + Live-Status)
- `SHOW PROCESSLIST` — aktive Verbindungen anzeigen

#### Phase 55 — DATE/TIME Datentypen
- Neue Datentypen: `DATE`, `TIME`, `DATETIME`, `TIMESTAMP`
- Funktionen: `NOW()`, `CURDATE()`, `CURTIME()`, `DATEDIFF()`, `DATE_ADD()`, `DATE_SUB()`, `DATE_FORMAT()`
- Vergleiche und WHERE-Filter auf Datumswerten

#### Phase 56 — JSON Datentyp
- Neuer Datentyp: `JSON`
- Funktionen: `JSON_EXTRACT(col, '$.key')`, `JSON_SET(col, '$.key', val)`, `JSON_KEYS(col)`, `JSON_LENGTH(col)`, `JSON_CONTAINS(col, val)`, `JSON_TYPE(col)`, `JSON_VALID(col)`

#### Phase 57 — Backup/Restore
- `BACKUP DATABASE TO 'datei.sql'` — vollständiger SQL-Dump (mysqldump-kompatibel)
- `BACKUP TABLE tabellenname TO 'datei.sql'` — einzelne Tabelle
- `RESTORE DATABASE FROM 'datei.sql'` — Datenbankwiederherstellung
- `SHOW BACKUPS` — listet alle `.sql`-Dateien im Verzeichnis
- `DROP TABLE IF EXISTS` — Parser-Unterstützung
- Topologische Sortierung nach Foreign Keys beim Dump
- Auto-Dateiname wenn kein Pfad angegeben: `milansql_YYYYMMDD_HHMMSS.sql`

#### Phase 58 — Connection Pooling + BENCHMARK
- Thread Pool im TCP-Server (Standard: 10 Worker)
- `--pool-size N` und `--max-queue N` CLI-Parameter
- `BENCHMARK N sql` — führt SQL N-mal aus und zeigt min/max/avg/QPS
- `SHOW STATUS` erweitert: Pool Size, Active Workers, Queued Requests, Total Requests, Avg Query Time

#### Phase 53 — Client Libraries (ebenfalls in v1.1.0)
- Python Client: DB-API 2.0 (PEP 249), TCP + HTTP, `pip install -e clients/python`
- Node.js Client: HTTP REST, 0 externe Dependencies, `require('milansql')`

### Bugfixes
- `WHERE col = 'wert'` — Vergleich mit gespeicherten Werten ohne Anführungszeichen funktioniert jetzt korrekt (Quote-Stripping in `compareValues()`)
- Wiederhergestellte Werte werden ohne Anführungszeichen angezeigt (`dispatch_displayVal`)
- `SHOW STATUS` zeigt jetzt korrekt die Statusanzeige (war zuvor `SHOW TABLES`)

---

## [v1.0.0] — 2025-xx-xx

### Features (Phasen 1–53)

- SQL Core: DDL, DML, SELECT mit WHERE/ORDER BY/LIMIT/DISTINCT/LIKE/BETWEEN/IN/EXISTS
- Aggregation: COUNT, MIN, MAX, AVG, SUM mit GROUP BY/HAVING
- JOINs: INNER, LEFT, RIGHT, FULL OUTER, mehrfache JOINs
- Mengenoperationen: UNION, UNION ALL, INTERSECT, EXCEPT
- Subqueries: korreliert, skalare Subqueries, IN/NOT IN/EXISTS
- Constraints: NOT NULL, UNIQUE, DEFAULT, PRIMARY KEY, AUTO_INCREMENT, CHECK, FOREIGN KEY
- Transaktionen: BEGIN/COMMIT/ROLLBACK (WAL-basiert)
- Views, Trigger (BEFORE/AFTER, SIGNAL, NEW/OLD), Stored Procedures, Prepared Statements
- B-Tree Index (T=3), mehrspaltige Indizes, Full-Text Search (MATCH AGAINST, TF-Ranking)
- Window Functions: ROW_NUMBER, RANK, DENSE_RANK, SUM/AVG/COUNT/MIN/MAX OVER
- CTE (Common Table Expressions), CASE WHEN, CAST, COALESCE/IFNULL
- String-Funktionen: UPPER, LOWER, LENGTH, CONCAT, SUBSTR, TRIM, REPLACE
- Math-Funktionen: ABS, ROUND, MOD, POWER, SQRT, CEIL, FLOOR
- Benutzerverwaltung: CREATE USER, GRANT/REVOKE, CONNECT/DISCONNECT
- Schemas/Namespaces: CREATE/DROP/USE SCHEMA, Cross-Schema JOINs
- TCP/IP Server (Multi-Thread, Winsock2/POSIX) + TCP Client
- REST API (HTTP/JSON): GET/POST /query, /tables, /schemas, /status
- Cost-Based Query Optimizer, EXPLAIN
- Eigenes Binärformat (Format v7, XOR-Checksumme)
- GitHub Actions CI (Ubuntu + Windows), 41 automatisierte Tests, Benchmark

---

*MilanSQL — Eine relationale Datenbank-Engine in C++17, entwickelt von Mirwais Haidari.*
