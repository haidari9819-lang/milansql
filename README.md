# MilanSQL

Eine selbst gebaute relationale Datenbank in **C++17** — inspiriert von MariaDB/SQLite.  
Entwickelt von **Mirwais Haidari** als Lernprojekt, Phase für Phase aufgebaut.

---

## Features

| Kategorie | Unterstützte Befehle |
|-----------|---------------------|
| **DDL** | `CREATE TABLE`, `DROP TABLE`, `ALTER TABLE` (ADD/DROP/RENAME COLUMN) |
| **DML** | `INSERT INTO`, `SELECT`, `UPDATE SET`, `DELETE FROM`, `TRUNCATE TABLE` |
| **Views** | `CREATE VIEW … AS SELECT`, `DROP VIEW`, `SELECT * FROM view` |
| **Constraints** | `NOT NULL`, `UNIQUE`, `DEFAULT`, `PRIMARY KEY`, `AUTO_INCREMENT`, `CHECK` |
| **Foreign Keys** | `FOREIGN KEY … REFERENCES`, `ON DELETE RESTRICT / CASCADE / SET NULL` |
| **SELECT** | `WHERE`, `ORDER BY [DESC]`, `LIMIT`, `DISTINCT`, `LIKE`, `IS NULL`, `BETWEEN`, `IN`, Subqueries, `EXISTS` |
| **Aggregation** | `COUNT(*)`, `MIN`, `MAX`, `AVG`, `SUM`, `GROUP BY`, `HAVING` |
| **JOINs** | `INNER JOIN`, `LEFT JOIN`, mehrfache JOINs |
| **Indizes** | `CREATE INDEX` / `DROP INDEX` (B-Tree, T=3) |
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
SELECT [DISTINCT] * | col,... FROM name
  [WHERE col op val [AND|OR ...]]
  [ORDER BY col [DESC]]
  [LIMIT n]
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

---

## Lizenz

MIT License — frei verwendbar für Lern- und Demozwecke.
