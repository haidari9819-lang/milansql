# MilanSQL

Eine selbst gebaute relationale Datenbank in **C++17** — inspiriert von MariaDB/SQLite.  
Entwickelt von **Mirwais Haidari** als Lernprojekt, Phase für Phase aufgebaut.

---

## Features

| Kategorie | Unterstützte Befehle |
|-----------|---------------------|
| **DDL** | `CREATE TABLE`, `DROP TABLE`, `ALTER TABLE` (ADD/DROP/RENAME COLUMN) |
| **DML** | `INSERT INTO` (single, multi-row, SELECT), `INSERT OR REPLACE`, `INSERT OR IGNORE`, `SELECT`, `UPDATE SET`, `DELETE FROM`, `TRUNCATE TABLE` |
| **String-Funktionen** | `UPPER`, `LOWER`, `LENGTH`, `CONCAT`, `SUBSTR`, `TRIM`, `REPLACE` in SELECT mit AS alias |
| **Typumwandlung** | `CAST(expr AS INT\|REAL\|TEXT)` in SELECT und WHERE (kombinierbar mit anderen Funktionen) |
| **Math-Funktionen** | `ABS`, `ROUND`, `MOD`, `POWER`, `SQRT`, `CEIL`, `FLOOR` in SELECT mit AS alias |
| **NULL-Funktionen** | `COALESCE(v1, v2, ...)`, `IFNULL(col, default)` in SELECT |
| **Views** | `CREATE VIEW … AS SELECT`, `DROP VIEW`, `SELECT * FROM view` |
| **Constraints** | `NOT NULL`, `UNIQUE`, `DEFAULT`, `PRIMARY KEY`, `AUTO_INCREMENT`, `CHECK` |
| **Foreign Keys** | `FOREIGN KEY … REFERENCES`, `ON DELETE RESTRICT / CASCADE / SET NULL` |
| **SELECT** | `WHERE`, `ORDER BY col1 [ASC\|DESC], col2 [ASC\|DESC]`, `LIMIT n [OFFSET m]`, `DISTINCT`, `LIKE`, `IS NULL`, `BETWEEN`, `IN`, Subqueries, `EXISTS` |
| **Aggregation** | `COUNT(*)`, `MIN`, `MAX`, `AVG`, `SUM`, `GROUP BY`, `HAVING` |
| **JOINs** | `INNER JOIN`, `LEFT JOIN`, `RIGHT JOIN`, `FULL [OUTER] JOIN`, mehrfache JOINs |
| **Mengen** | `UNION`, `UNION ALL`, `INTERSECT`, `EXCEPT` |
| **Indizes** | `CREATE INDEX` / `DROP INDEX` (B-Tree, T=3), mehrspaltige Indizes |
| **EXPLAIN** | `EXPLAIN SELECT ...` — Query-Plan (SCAN/INDEX/FILTER/JOIN/GROUP/AGGREGATE/SORT/LIMIT/PROJECT) |
| **Correlated Subqueries** | `WHERE col > (SELECT AVG(...) WHERE col = alias.col)`, `(SELECT COUNT(*) FROM ...) AS alias` in SELECT, `EXISTS` mit mehreren Bedingungen |
| **WITH / CTE** | `WITH name AS (SELECT ...), ... SELECT ...` (Common Table Expressions, mehrere CTEs) |
| **Window Functions** | `ROW_NUMBER()`, `RANK()`, `DENSE_RANK()`, `SUM/AVG/COUNT/MIN/MAX() OVER (PARTITION BY col ORDER BY col)` |
| **Transaktionen** | `BEGIN`, `COMMIT`, `ROLLBACK` (WAL-basiert) |
| **Introspection** | `DESCRIBE`, `SHOW TABLES`, `SHOW CREATE TABLE`, `SHOW INDEXES`, `STATUS` |
| **Persistenz** | Eigenes Binärformat (`database.milan`, Format v7) mit Checksumme |

---

## Projektstruktur

```
milansql/
├── CMakeLists.txt              # Build-Konfiguration (CMake 3.16+)
├── README.md
└── src/
    ├── main.cpp                # REPL — Eingabe, Ausgabe, Dispatch
    ├── engine/
    │   ├── engine.hpp          # Kern-Engine: Tabellen, Rows, Constraints,
    │   │                       # FK-Cascade, Aggregation, JOINs, Subqueries,
    │   │                       # Transaktionen (WAL), Views
    │   └── btree.hpp           # B-Tree Index (In-Memory, T=3)
    ├── parser/
    │   └── parser.hpp          # SQL-Tokenizer & vollständiger Parser
    └── storage/
        └── storage.hpp         # Binäres Dateiformat (MilanBinaryStorage)
```

---

## Architektur

```
Eingabe (stdin)
      │
      ▼
┌─────────────┐
│   Parser    │  Tokenisiert SQL → ParsedCommand (Typ, Tabelle, WHERE, ...)
└──────┬──────┘
       │
       ▼
┌─────────────┐
│   Engine    │  Führt Befehl aus (RAM), prüft Constraints, FK-Cascade,
│             │  Transaktions-Log (WAL), View-Materialisierung
└──────┬──────┘
       │
       ▼
┌─────────────┐
│   Storage   │  Serialisiert Engine-Zustand → database.milan (Binär)
│             │  Format v7: Magic + Version + Checksum + Tables + Views
└─────────────┘
```

### Binärformat (`database.milan`)

| Offset | Größe | Inhalt |
|--------|-------|--------|
| 0–7 | 8 B | Magic `MILANDB1` |
| 8–9 | 2 B | Format-Version (aktuell: 7) |
| 10–13 | 4 B | Page Count (reserviert) |
| 14–15 | 2 B | XOR-Checksumme über Data-Section |
| 16+ | — | Data-Section: Tabellen, dann Views |

---

## Bauen

### Voraussetzungen

- **CMake** ≥ 3.16
- **C++17**-Compiler (GCC ≥ 7, Clang ≥ 5, MSVC 2017+)
- Auf Windows: [MSYS2](https://www.msys2.org/) mit `ucrt64`-Toolchain empfohlen

### Windows (MSYS2 / MinGW)

```powershell
# Einmalig: MSYS2 installieren, dann im UCRT64-Terminal:
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake ninja

# Repository klonen und bauen
git clone https://github.com/MirwaisHaidari/milansql.git
cd milansql
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

### Linux / macOS

```bash
git clone https://github.com/MirwaisHaidari/milansql.git
cd milansql
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/milansql
```

---

## Starten

```
./build/milansql        # Linux/macOS
build\milansql.exe      # Windows
```

Die Datenbank wird automatisch als `database.milan` im Arbeitsverzeichnis gespeichert.

---

## Beispiel-Session

```sql
CREATE TABLE produkte (
  id INT PRIMARY KEY AUTO_INCREMENT,
  name TEXT NOT NULL,
  preis INT CHECK (preis >= 0),
  bestand INT DEFAULT 0
)

INSERT INTO produkte VALUES (NULL, Apfel, 150, 100)
INSERT INTO produkte VALUES (NULL, Banane, 80, 200)
INSERT INTO produkte VALUES (NULL, Mango, 320, 30)

SELECT * FROM produkte
-- ┌────┬────────┬───────┬─────────┐
-- │ id │ name   │ preis │ bestand │
-- ├────┼────────┼───────┼─────────┤
-- │ 1  │ Apfel  │ 150   │ 100     │
-- │ 2  │ Banane │ 80    │ 200     │
-- │ 3  │ Mango  │ 320   │ 30      │
-- └────┴────────┴───────┴─────────┘

SELECT * FROM produkte WHERE preis > 100 ORDER BY preis DESC

CREATE VIEW guenstige AS SELECT * FROM produkte WHERE preis < 200
SELECT * FROM guenstige

SELECT COUNT(*), AVG(preis) FROM produkte

UPDATE produkte SET preis=90 WHERE id=1
DELETE FROM produkte WHERE bestand < 50

SHOW TABLES
SHOW CREATE TABLE produkte
STATUS
```

---

## SQL-Referenz

### Tabellen

```sql
CREATE TABLE name (
  col TYP [PRIMARY KEY] [AUTO_INCREMENT] [NOT NULL] [UNIQUE]
          [DEFAULT wert] [CHECK (col op wert)],
  ...
  [FOREIGN KEY (col) REFERENCES tabelle(col) [ON DELETE CASCADE|SET NULL|RESTRICT]]
)

ALTER TABLE name ADD COLUMN col TYP
ALTER TABLE name DROP COLUMN col
ALTER TABLE name RENAME COLUMN alt TO neu
DROP TABLE name
TRUNCATE TABLE name
DESCRIBE name
SHOW CREATE TABLE name
```

### Daten

```sql
INSERT INTO name VALUES (v1, v2, ...)
INSERT OR REPLACE INTO name VALUES (v1, v2, ...)  -- PK/UNIQUE Konflikt: löschen + neu einfügen
INSERT OR IGNORE  INTO name VALUES (v1, v2, ...)  -- PK/UNIQUE Konflikt: ignorieren
INSERT INTO name VALUES (v1, v2, ...) ON CONFLICT DO NOTHING  -- wie OR IGNORE
SELECT [DISTINCT] * | col,... FROM name
  [WHERE col op val [AND|OR ...]]
  [ORDER BY col1 [ASC|DESC] [, col2 [ASC|DESC] ...]]
  [LIMIT n [OFFSET m]]
SELECT COUNT(*) | MIN(col) | MAX(col) | AVG(col) | SUM(col) FROM name [WHERE ...]
SELECT col, AGG(col) FROM name GROUP BY col [HAVING AGG(col) op val]
SELECT * FROM t1 [LEFT] JOIN t2 ON t1.col = t2.col
UPDATE name SET col=val [, col=val ...] [WHERE col = val]
DELETE FROM name [WHERE col = val]
```

### Views

```sql
CREATE VIEW name AS SELECT ...
DROP VIEW name
SELECT * FROM viewname [WHERE ...]
DESCRIBE viewname
```

### Transaktionen

```sql
BEGIN
-- DML-Befehle ...
COMMIT    -- oder ROLLBACK
```

### Info

```sql
SHOW TABLES
SHOW INDEXES FROM name
SHOW CREATE TABLE name
STATUS
```

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
| 31 | CASE WHEN THEN ELSE END in SELECT (mit AS alias) |
| 32 | String-Funktionen: UPPER, LOWER, LENGTH, CONCAT, SUBSTR, TRIM, REPLACE |
| 33 | Math-Funktionen: ABS, ROUND, MOD, POWER, SQRT, CEIL, FLOOR |
| 34 | NULL-Funktionen: COALESCE, IFNULL |
| 35 | Composite Indexes: `CREATE INDEX name ON tabelle (col1, col2, ...)`, `SHOW INDEXES` zeigt alle Spalten |
| 36 | EXPLAIN: `EXPLAIN SELECT ...` zeigt Query-Plan (Schritt, Operation, Tabelle, Details, Index) |
| 37 | Correlated Subqueries: `WHERE col > (SELECT AVG(...) WHERE col = alias.col)`, Scalar Subquery in SELECT `(SELECT COUNT(*) FROM ...) AS alias`, EXISTS mit mehreren Bedingungen |
| 38 | Multi-Column ORDER BY: `ORDER BY col1 ASC, col2 DESC`; LIMIT mit OFFSET: `LIMIT n OFFSET m` |
| 39 | UPSERT: `INSERT OR REPLACE INTO t VALUES (...)` — Konflikt→löschen+einfügen; `INSERT OR IGNORE INTO t VALUES (...)` — Konflikt→ignorieren; `ON CONFLICT DO NOTHING` |
| 40 | CAST: `CAST(expr AS INT\|REAL\|TEXT)` in SELECT und WHERE |
| 41 | WITH / CTE: `WITH name AS (SELECT ...)` — Common Table Expressions, mehrere CTEs, UNION |
| 42 | Window Functions: `ROW_NUMBER/RANK/DENSE_RANK/SUM/AVG/COUNT/MIN/MAX OVER (PARTITION BY ... ORDER BY ...)` |

---

## Testing (Phase 39.5)

### Test-Suite ausführen

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/milansql_tests        # Linux/macOS
build\milansql_tests.exe      # Windows
```

Die Test-Suite enthält **41 automatisierte Tests** in 10 Gruppen, die alle Kern-Features abdecken:
DDL/DML, Aggregation, JOINs, Transaktionen, Constraints, Foreign Keys + CASCADE,
Mengenoperationen (UNION/INTERSECT/EXCEPT), CASE WHEN, WITH/CTE und B-Tree Indizes.
Alle Tests laufen in-memory — keine Datenbankdatei wird angelegt.

### Benchmark ausführen

```bash
./build/milansql_bench        # Linux/macOS
build\milansql_bench.exe      # Windows
```

Typische Ergebnisse auf Entwicklungshardware:

| Messung | Zeit |
|---------|------|
| 10.000 INSERTs | ~120ms |
| SELECT ohne Index (Full Scan) | ~2ms |
| SELECT mit B-Tree Index | <1ms |
| 10.000 Index-SELECTs | ~8ms |

### CI/CD

GitHub Actions führt Build und Tests automatisch auf **Ubuntu** und **Windows** aus
bei jedem Push und Pull Request auf `main` (`.github/workflows/ci.yml`).

---

## Lizenz

MIT License — frei verwendbar für Lern- und Demozwecke.
