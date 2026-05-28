# MilanSQL

![Version](https://img.shields.io/badge/version-v1.0.0-brightgreen)
![CI](https://github.com/haidari9819-lang/milansql/actions/workflows/ci.yml/badge.svg)
![License](https://img.shields.io/badge/license-MIT-blue)
![Language](https://img.shields.io/badge/language-C%2B%2B17-orange)

Eine selbst gebaute relationale Datenbank in **C++17** — inspiriert von MariaDB/SQLite.  
Entwickelt von **Mirwais Haidari**, Phase für Phase aufgebaut. **53 Features, 0 externe Abhängigkeiten.**

---

## Quick Start

```bash
# 1. Repository klonen
git clone https://github.com/haidari9819-lang/milansql
cd milansql

# 2. Bauen (Linux/macOS)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build

# 3. Starten
./build/milansql
```

```powershell
# Windows (MSYS2 UCRT64)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
.\build\milansql.exe
```

---

## REPL

```
  ╔══════════════════════════════════════════╗
  ║        === MilanSQL v1.0.0 ===           ║
  ║   Built with <3 by Mirwais Haidari       ║
  ║  Type 'help' for commands, 'exit' to quit║
  ╚══════════════════════════════════════════╝

milansql> CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name TEXT, score INT)
  Tabelle 'users' erstellt.

milansql> INSERT INTO users VALUES (NULL, Alice, 95)
  1 Zeile(n) eingefuegt.

milansql> SELECT * FROM users WHERE score > 80 ORDER BY score DESC
  ┌────┬───────┬───────┐
  │ id │ name  │ score │
  ├────┼───────┼───────┤
  │ 1  │ Alice │ 95    │
  └────┴───────┴───────┘
  1 Zeile(n).

milansql> STATUS
```

---

## Features

### SQL Core
| Kategorie | Befehle |
|-----------|---------|
| **DDL** | `CREATE TABLE`, `DROP TABLE`, `ALTER TABLE` (ADD/DROP/RENAME COLUMN), `TRUNCATE TABLE` |
| **DML** | `INSERT INTO` (single, multi-row, `INSERT INTO … SELECT`), `INSERT OR REPLACE`, `INSERT OR IGNORE`, `ON CONFLICT DO NOTHING`, `UPDATE SET`, `DELETE FROM` |
| **SELECT** | `WHERE`, `ORDER BY col [ASC\|DESC], …`, `LIMIT n [OFFSET m]`, `DISTINCT`, `LIKE`, `IS NULL`, `BETWEEN`, `IN`, `EXISTS` |
| **Aggregation** | `COUNT(*)`, `MIN`, `MAX`, `AVG`, `SUM` mit `GROUP BY` und `HAVING` |
| **JOINs** | `INNER JOIN`, `LEFT JOIN`, `RIGHT JOIN`, `FULL OUTER JOIN`, mehrfache JOINs |
| **Mengen** | `UNION`, `UNION ALL`, `INTERSECT`, `EXCEPT` |
| **Subqueries** | Correlated Subqueries, Scalar Subquery in SELECT, `IN`/`NOT IN`, `EXISTS` |

### Funktionen & Ausdrücke
| Kategorie | Befehle |
|-----------|---------|
| **String** | `UPPER`, `LOWER`, `LENGTH`, `CONCAT`, `SUBSTR`, `TRIM`, `REPLACE` |
| **Math** | `ABS`, `ROUND`, `MOD`, `POWER`, `SQRT`, `CEIL`, `FLOOR` |
| **NULL** | `COALESCE(v1, v2, …)`, `IFNULL(col, default)` |
| **Typumwandlung** | `CAST(expr AS INT\|REAL\|TEXT)` in SELECT und WHERE |
| **CASE** | `CASE WHEN … THEN … ELSE … END` in SELECT |
| **Window Functions** | `ROW_NUMBER()`, `RANK()`, `DENSE_RANK()`, `SUM/AVG/COUNT/MIN/MAX() OVER (PARTITION BY … ORDER BY …)` |
| **CTE** | `WITH name AS (SELECT …), … SELECT …` — mehrere CTEs, UNION |

### Constraints & Integrität
| Feature | Details |
|---------|---------|
| **Constraints** | `NOT NULL`, `UNIQUE`, `DEFAULT`, `PRIMARY KEY`, `AUTO_INCREMENT`, `CHECK` |
| **Foreign Keys** | `FOREIGN KEY … REFERENCES` mit `ON DELETE CASCADE / SET NULL / RESTRICT` |
| **Transaktionen** | `BEGIN / COMMIT / ROLLBACK` (WAL-basiert) |

### Erweiterte Features
| Feature | Details |
|---------|---------|
| **Trigger** | `CREATE TRIGGER BEFORE/AFTER INSERT/UPDATE/DELETE … FOR EACH ROW BEGIN … END`, `SIGNAL`, `NEW`/`OLD`, `DROP TRIGGER`, `SHOW TRIGGERS` |
| **Stored Procedures** | `CREATE PROCEDURE / CALL / DROP PROCEDURE / SHOW PROCEDURES` |
| **Prepared Statements** | `PREPARE / EXECUTE / DEALLOCATE PREPARE / SHOW PREPARED` |
| **Views** | `CREATE VIEW … AS SELECT`, `DROP VIEW`, `SELECT * FROM view` |
| **Indizes** | `CREATE INDEX` / `DROP INDEX` (B-Tree, T=3), mehrspaltige Indizes |
| **Full-Text Search** | `CREATE FULLTEXT INDEX`, `MATCH(col) AGAINST ('term')`, TF-Ranking |
| **Query Optimizer** | Cost-based Join-Reihenfolge, Index-Auswahl, Predicate Pushdown |
| **EXPLAIN** | `EXPLAIN SELECT …` — Query-Plan mit Optimizer-Entscheidungen |
| **Schemas** | `CREATE SCHEMA`, `USE schema`, Cross-Schema JOINs (`shop.produkte`) |
| **Benutzerverwaltung** | `CREATE USER`, `GRANT/REVOKE priv ON tbl TO/FROM user`, `CONNECT user pwd` |

### Server & Clients
| Modus | Befehl |
|-------|--------|
| **TCP Server** | `milansql --server --port 4406` — Multi-Client, Thread-Safe |
| **REST API** | `milansql --http --port 8080` — HTTP/JSON Interface |
| **Python Client** | DB-API 2.0 (PEP 249), TCP + HTTP, `pip install -e clients/python` |
| **Node.js Client** | HTTP REST, 0 externe Deps, `require('milansql')` |

### Introspection & Persistenz
| Feature | Details |
|---------|---------|
| **Introspection** | `DESCRIBE`, `SHOW TABLES`, `SHOW CREATE TABLE`, `SHOW INDEXES`, `STATUS` |
| **Persistenz** | Eigenes Binärformat (`database.milan`, Format v7) mit XOR-Checksumme |

---

## Server-Modi

### TCP Server + Client

```bash
# Server starten
./build/milansql --server --port 4406

# In zweitem Terminal: Client verbinden
./build/milansql --client --port 4406
```

### REST API (HTTP)

```bash
# Server starten
./build/milansql --http --port 8080
```

```powershell
# SQL ausführen (POST)
Invoke-WebRequest -Uri "http://localhost:8080/query" `
  -Method POST -ContentType "application/json" `
  -Body '{"sql":"SELECT * FROM users"}'

# SQL ausführen (GET)
Invoke-WebRequest -Uri "http://localhost:8080/query?sql=SELECT+*+FROM+users"

# Metadaten
Invoke-WebRequest -Uri "http://localhost:8080/tables"
Invoke-WebRequest -Uri "http://localhost:8080/status"
```

JSON-Antwort bei SELECT:
```json
{
  "success": true,
  "columns": ["id", "name", "score"],
  "rows": [[1, "Alice", 95], [2, "Bob", 82]],
  "rowCount": 2,
  "executionTime": "0.1ms"
}
```

---

## Client Libraries

### Python (DB-API 2.0 / PEP 249)

```bash
pip install -e clients/python
```

```python
import milansql

# TCP Client (DB-API 2.0)
with milansql.connect(host='localhost', port=4406) as conn:
    cur = conn.cursor()
    cur.execute("CREATE TABLE IF NOT EXISTS produkte (id INT PRIMARY KEY AUTO_INCREMENT, name TEXT, preis INT)")
    cur.execute("INSERT INTO produkte VALUES (NULL, %s, %s)", ("Laptop", 1200))
    cur.execute("INSERT INTO produkte VALUES (NULL, %s, %s)", ("Maus", 25))
    cur.execute("SELECT * FROM produkte WHERE preis > %s", (100,))
    for row in cur.fetchall():
        print(row)   # (1, 'Laptop', 1200)

# HTTP Client
with milansql.connect_http(host='localhost', port=8080) as conn:
    result = conn.query("SELECT * FROM produkte")
    print(result.columns)   # ['id', 'name', 'preis']
    print(result.rows)      # [[1, 'Laptop', 1200], ...]
```

### Node.js

```bash
# Kein npm install nötig — 0 Dependencies
node clients/nodejs/examples/basic.js
```

```js
const milansql = require('./clients/nodejs');

const conn = milansql.connect({ host: 'localhost', port: 8080 });

const result = await conn.query('SELECT * FROM users ORDER BY score DESC');
console.log(result.columns);  // ['id', 'name', 'score']
console.log(result.rows);     // [[1, 'Alice', 95], ...]

const tables  = await conn.tables();
const status  = await conn.status();
```

---

## Projektstruktur

```
milansql/
├── CMakeLists.txt
├── .github/workflows/ci.yml    # GitHub Actions (Ubuntu + Windows)
├── src/
│   ├── main.cpp                # REPL + Argument-Parsing (--server/--client/--http)
│   ├── dispatch.hpp            # SQL Command Dispatcher
│   ├── engine/
│   │   ├── engine.hpp          # Kern-Engine: Tabellen, Constraints, JOINs,
│   │   │                       # Transaktionen (WAL), Views, Trigger, Procedures,
│   │   │                       # Window Functions, Full-Text Search, Schemas
│   │   └── btree.hpp           # B-Tree Index (In-Memory, T=3)
│   ├── parser/
│   │   └── parser.hpp          # SQL-Tokenizer & Parser → ParsedCommand
│   ├── storage/
│   │   └── storage.hpp         # Binärformat MilanBinaryStorage (Format v7)
│   ├── optimizer/
│   │   └── optimizer.hpp       # Cost-Based Query Optimizer
│   ├── server/
│   │   ├── server.hpp          # TCP Server (Winsock2/POSIX, Multi-Thread)
│   │   ├── client.hpp          # TCP Client (REPL über Netzwerk)
│   │   └── http_server.hpp     # HTTP/JSON REST API Server
│   └── tests/
│       ├── milansql_tests.cpp  # 41 automatisierte Tests
│       └── benchmark.cpp       # Performance-Benchmark
└── clients/
    ├── python/                 # Python Package (DB-API 2.0 / PEP 249)
    │   ├── milansql/
    │   │   ├── connection.py   # TCP Client + Cursor
    │   │   ├── http_client.py  # HTTP Client
    │   │   └── exceptions.py
    │   ├── setup.py
    │   └── examples/
    └── nodejs/                 # Node.js Client (HTTP)
        ├── index.js
        └── examples/
```

---

## Bauen

### Voraussetzungen

- **CMake** ≥ 3.16
- **C++17**-Compiler (GCC ≥ 7, Clang ≥ 5, MSVC 2017+)
- Windows: [MSYS2](https://www.msys2.org/) UCRT64 empfohlen

### Linux / macOS

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/milansql
```

### Windows (MSYS2 UCRT64)

```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake ninja
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/milansql.exe
```

---

## Testing & CI

```bash
# Test-Suite (41 Tests)
./build/milansql_tests

# Benchmark
./build/milansql_bench
```

| Messung | Zeit |
|---------|------|
| 10.000 INSERTs | ~120ms |
| SELECT Full Scan | ~2ms |
| SELECT mit B-Tree Index | <1ms |
| 10.000 Index-SELECTs | ~8ms |

GitHub Actions führt Build und Tests automatisch auf **Ubuntu** und **Windows** aus bei jedem Push auf `main`.

---

## SQL-Kurzreferenz

```sql
-- DDL
CREATE TABLE name (id INT PRIMARY KEY AUTO_INCREMENT, name TEXT NOT NULL, preis INT DEFAULT 0)
ALTER TABLE name ADD COLUMN col TYP
DROP TABLE name

-- DML
INSERT INTO name VALUES (NULL, 'Wert', 100)
INSERT INTO name VALUES (NULL, 'A', 1), (NULL, 'B', 2)   -- Multi-Row
INSERT OR REPLACE INTO name VALUES (...)
UPDATE name SET col=val WHERE id=1
DELETE FROM name WHERE col=val

-- SELECT
SELECT DISTINCT col1, col2 FROM t WHERE col > 10
SELECT * FROM t ORDER BY col1 DESC, col2 ASC LIMIT 10 OFFSET 20
SELECT COUNT(*), AVG(preis), MAX(preis) FROM t GROUP BY kategorie HAVING COUNT(*) > 1
SELECT * FROM t1 INNER JOIN t2 ON t1.id = t2.fk_id
SELECT * FROM t1 LEFT JOIN t2 ON t1.id = t2.id

-- Erweitert
WITH cte AS (SELECT * FROM t WHERE val > 100)
SELECT * FROM cte ORDER BY val

SELECT name, ROW_NUMBER() OVER (PARTITION BY kategorie ORDER BY preis DESC) AS rang FROM produkte

SELECT CAST(preis AS TEXT) AS preis_str, UPPER(name), COALESCE(notiz, 'k.A.') FROM t

-- Trigger
CREATE TRIGGER before_insert BEFORE INSERT ON orders FOR EACH ROW
BEGIN
  IF NEW.preis < 0 THEN SIGNAL 'Preis darf nicht negativ sein' END IF
END

-- Prozeduren
CREATE PROCEDURE add_user(p_name TEXT, p_score INT)
BEGIN
  INSERT INTO users VALUES (NULL, p_name, p_score)
END
CALL add_user(Alice, 100)

-- Server
SHOW TABLES
SHOW CREATE TABLE name
SHOW INDEXES FROM name
SHOW TRIGGERS ON name
SHOW PROCEDURES
STATUS
```

---

## Binärformat (`database.milan`)

| Offset | Größe | Inhalt |
|--------|-------|--------|
| 0–7 | 8 B | Magic `MILANDB1` |
| 8–9 | 2 B | Format-Version (v7) |
| 10–13 | 4 B | Page Count (reserviert) |
| 14–15 | 2 B | XOR-Checksumme |
| 16+ | — | Tabellen + Views |

---

## Entwicklungs-Phasen

| Phase | Feature |
|-------|---------|
| 1 | Grundstruktur, REPL, CREATE TABLE, INSERT, SELECT |
| 2 | Binäres Dateiformat (MilanBinaryStorage) |
| 3 | WHERE-Filter, DELETE |
| 4 | UPDATE SET |
| 5 | DESCRIBE, DROP TABLE |
| 6 | ALTER TABLE (ADD/DROP/RENAME COLUMN) |
| 7 | SHOW TABLES, SHOW INDEXES |
| 8 | ORDER BY, LIMIT |
| 9 | Multi-WHERE (AND/OR), LIKE |
| 10 | COUNT(*), MIN, MAX, AVG, SUM |
| 11 | SELECT mit Spaltenauswahl, DISTINCT |
| 12 | B-Tree Index (CREATE INDEX / DROP INDEX) |
| 13 | INNER JOIN, LEFT JOIN |
| 14 | GROUP BY + HAVING |
| 15 | Subqueries (IN/NOT IN) |
| 16 | NULL-Unterstützung (IS NULL / IS NOT NULL) |
| 17 | Transaktionen (BEGIN / COMMIT / ROLLBACK, WAL) |
| 18 | DEFAULT, UNIQUE, NOT NULL, PRIMARY KEY Constraints |
| 19 | AUTO_INCREMENT |
| 20 | Foreign Keys (FOREIGN KEY … REFERENCES) |
| 21 | ON DELETE CASCADE / SET NULL / RESTRICT |
| 22 | TRUNCATE TABLE, Multi-Column UPDATE |
| 23 | CHECK Constraints |
| 24 | Views (CREATE VIEW / DROP VIEW / SELECT FROM view) |
| 25 | SHOW TABLES tabellarisch, STATUS, SHOW CREATE TABLE |
| 26 | README + GitHub |
| 27 | Multi-row INSERT — `INSERT INTO t VALUES (...),(...),...` |
| 28 | INSERT INTO ... SELECT — Ergebnis einer Abfrage einfügen |
| 29 | RIGHT JOIN + FULL OUTER JOIN |
| 30 | UNION / UNION ALL / INTERSECT / EXCEPT |
| 31 | CASE WHEN THEN ELSE END in SELECT |
| 32 | String-Funktionen: UPPER, LOWER, LENGTH, CONCAT, SUBSTR, TRIM, REPLACE |
| 33 | Math-Funktionen: ABS, ROUND, MOD, POWER, SQRT, CEIL, FLOOR |
| 34 | NULL-Funktionen: COALESCE, IFNULL |
| 35 | Composite Indexes: mehrspaltige Indizes |
| 36 | EXPLAIN: Query-Plan (SCAN/INDEX/FILTER/JOIN/GROUP/SORT/LIMIT/PROJECT) |
| 37 | Correlated Subqueries, Scalar Subquery in SELECT, EXISTS |
| 38 | Multi-Column ORDER BY, LIMIT mit OFFSET |
| 39 | UPSERT: INSERT OR REPLACE / INSERT OR IGNORE / ON CONFLICT DO NOTHING |
| 39.5 | Stabilisierung: Compiler-Warnings, 41 Tests, GitHub Actions CI, Benchmark |
| 40 | CAST: `CAST(expr AS INT\|REAL\|TEXT)` in SELECT und WHERE |
| 41 | WITH / CTE: Common Table Expressions, mehrere CTEs |
| 42 | Window Functions: ROW_NUMBER, RANK, DENSE_RANK, SUM/AVG/COUNT/MIN/MAX OVER |
| 43 | Trigger: BEFORE/AFTER INSERT/UPDATE/DELETE, SIGNAL, NEW/OLD |
| 44 | Stored Procedures: CREATE PROCEDURE / CALL / DROP PROCEDURE |
| 45 | Prepared Statements: PREPARE / EXECUTE / DEALLOCATE PREPARE |
| 46 | Benutzerverwaltung: CREATE USER / GRANT / REVOKE / CONNECT / DISCONNECT |
| 47 | TCP/IP Server: `--server/--client --port N`, Multi-Thread, Winsock2/POSIX |
| 48 | Cost-Based Query Optimizer: Join-Reihenfolge, Index-Auswahl, EXPLAIN |
| 49/50 | Full-Text Search: FULLTEXT INDEX, MATCH AGAINST, TF-Ranking |
| 51 | SCHEMA / Namespaces: CREATE/DROP/SHOW/USE SCHEMA, Cross-Schema JOINs |
| 52 | REST API: `--http --port 8080`, GET/POST /query, /tables, /schemas, /status |
| 53 | Client Libraries: Python (DB-API 2.0/PEP 249, TCP+HTTP), Node.js (HTTP) |
| 54 | Query Cache (LRU/TTL), EXPLAIN ANALYZE, Web Dashboard, SHOW PROCESSLIST |
| 55 | DATE/TIME Datentypen: DATE, TIME, DATETIME, TIMESTAMP, NOW(), CURDATE(), DATEDIFF(), DATE_ADD(), DATE_FORMAT() |
| 56 | JSON Datentyp: JSON_EXTRACT, JSON_SET, JSON_KEYS, JSON_LENGTH, JSON_CONTAINS, JSON_TYPE |
| 57 | Backup/Restore: BACKUP DATABASE/TABLE TO, RESTORE FROM, SHOW BACKUPS, SQL-Dump Format |
| 58 | Connection Pooling (Thread Pool, --pool-size), BENCHMARK Command, SHOW STATUS erweitert |

---

## Lizenz

MIT License — frei verwendbar für Lern- und Demozwecke.
