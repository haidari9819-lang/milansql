# Changelog

All notable changes to MilanSQL are documented in this file.

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
