# MilanSQL Community

## Show HN Post (copy-paste ready)

**Title:**
Show HN: MilanSQL — I built a relational database engine from scratch in C++17 (100 phases, MySQL/PostgreSQL compatible)

**Body:**
Over the past week I built MilanSQL, a complete relational database engine from scratch in pure C++17 with zero external dependencies.

What it can do:
- Full SQL: SELECT/JOIN/GROUP BY/Window Functions/CTEs/Recursive CTEs
- 5 network protocols: native TCP, MySQL Wire, PostgreSQL Wire, REST API, GraphQL
- MVCC transactions, WAL crash recovery, Savepoints
- Physical + Logical replication (Master/Slave + Pub/Sub)
- B-Tree indexes, Full-Text Search, Spatial (Haversine)
- Column Store for OLAP analytics
- Data compression: LZ4/RLE/Dictionary
- Change Data Capture (CDC)
- Event Scheduler, Row-Level Security, Triggers (ROW + STATEMENT)
- Python (DB-API 2.0) + Node.js clients

Performance: 86,220 INSERT ops/sec, 223 automated tests passing.

GitHub: https://github.com/haidari9819-lang/milansql

Built as a learning project to deeply understand how databases work internally.

---

## LinkedIn Post (copy-paste ready)

🚀 Nach 100 Entwicklungsphasen ist MilanSQL v4.0.0 fertig!

Ich habe eine vollständige relationale Datenbank-Engine von Grund auf in C++17 gebaut — ohne eine einzige externe Library.

Was MilanSQL kann:
✅ Vollständiges SQL (JOINs, Window Functions, CTEs, Trigger)
✅ 5 Netzwerk-Protokolle (MySQL, PostgreSQL, REST, GraphQL)
✅ MVCC Transaktionen + WAL + Crash Recovery
✅ Master/Slave Replikation + Logical Replication
✅ 86.220 INSERT ops/sec | 223 automatisierte Tests
✅ 0 externe Dependencies | Pure C++17

GitHub: https://github.com/haidari9819-lang/milansql

#Database #CPlusPlus #OpenSource #SystemsProgramming
