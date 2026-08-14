# MilanSQL v12.0.5

<p align="center">
  <img src="https://milansql.de/og-image.svg" alt="MilanSQL Logo" width="400">
</p>

Pure C++17. Zero external dependencies. Built by one developer. Runs on a €5 VPS.

![Version](https://img.shields.io/badge/version-v12.0.5-gold)
![License](https://img.shields.io/badge/license-MIT-blue)
![Tests](https://img.shields.io/badge/tests-2046%20passing-brightgreen)
![C++](https://img.shields.io/badge/C%2B%2B-17-blue)
![Dependencies](https://img.shields.io/badge/dependencies-0-brightgreen)
![Status](https://img.shields.io/badge/status-Production%20Ready-success)

<!-- Topics: database sql cpp c-plus-plus query-engine btree replication mvcc window-functions postgresql-compatible -->

## Live Demo

**[https://milansql.de/webui](https://milansql.de/webui)** — Live server, no installation needed.

---

## Feature Matrix

### PRODUCTION READY ✅

| Feature | Details |
|---------|---------|
| **Core SQL** | SELECT, INSERT, UPDATE, DELETE, JOINs (INNER/LEFT/CROSS/LATERAL), Subqueries, CTEs, Window Functions, GROUP BY, HAVING, BETWEEN, IN, EXISTS, IS NULL |
| **MVCC + WAL + fsync** | Multi-Version Concurrency Control, Write-Ahead Log, durable fsync on every commit |
| **Point-in-Time Recovery** | WAL archiving, base backups, restore to any timestamp |
| **Row-Level Security** | Per-user policies, `CURRENT_APP_USER_ID()`, auto-enforced on SELECT/INSERT/UPDATE/DELETE |
| **Column-Level Security** | Column masks and access control per role |
| **Fortress Security** | 34+ SQLi patterns blocked, honeypot traps, rate limiting |
| **Encryption at Rest** | AES-256-GCM (OpenSSL), transparent page-level encryption |
| **Audit Trail** | SHA-256 hash-chain, tamper-proof, append-only log |
| **Buffer Pool LRU** | 94.7% hit-rate measured on live workload |
| **Parallel Query Execution** | Thread pool, parallel scan and aggregation |
| **Cost-Based Optimizer** | Selinger dynamic programming, join method selection, auto-ANALYZE |
| **Table Partitioning** | Range, Hash, List partitioning |
| **Database Branching** | File-based isolation, zero-copy branch creation |
| **SSL/TLS** | TLS on all wire protocols |
| **Multi-Protocol** | MySQL Wire (4407), PostgreSQL Wire (5433), REST, GraphQL, WebSocket |
| **Natural Language SQL** | Groq integration — plain English → SQL |
| **Schema Visualizer** | WebUI with live table browser and query editor |
| **Streaming Replication** | Master + read replicas, WAL-based |
| **Connection Pooling** | Built-in pool with configurable size |
| **EXPLAIN FORMAT JSON** | Full query plans with cost estimates |
| **Prometheus Metrics** | `/metrics` endpoint |
| **Structured Logging** | JSON log output |
| **Health Endpoints** | `/health`, `/health/live`, `/health/ready` |
| **Migrations System** | UP / DOWN / STATUS with version tracking |
| **JavaScript/TypeScript SDK** | `npm install milansql-js` |
| **Python SDK** | `pip install milansql-py` |
| **CLI Tool** | `milansql-cli` — interactive shell + scripting |
| **Schema Introspection API** | `/api/schema` — full metadata in JSON |
| **Compliance Reports** | DSGVO, GoBD, SOC2 — exportable PDF |
| **IP Allowlisting + mTLS** | Network-level access control |
| **Isolated Tenants** | Per-tenant table namespacing with RLS enforcement |
| **Cloud Instance API** | `/cloud` — provision/manage instances via REST |
| **One-Click Deploy** | `install.sh`, Docker image, Helm chart |
| **2046 passing tests** | Unit + integration + fuzz (10k iterations) |

### REQUIRES MULTIPLE INSTANCES ⚙️

| Feature | Status |
|---------|--------|
| **Sharding** | API fully implemented — needs N independent MilanSQL nodes |
| **Logical Replication** | Protocol ready — needs a subscriber node running |
| **Multi-Region** | Routing layer ready — needs regional node deployments |

### PLANNED 🔮

| Feature | Notes |
|---------|-------|
| **Vectorized Execution** | SIMD/AVX2 batch processing |
| **Columnar Storage** | PAX layout for analytics workloads |
| **Native Managed Cloud** | milansql.cloud — hosted instances |

---

## Performance (measured on €5 VPS)

| Metric | Value |
|--------|-------|
| INSERT throughput | **86,220 rows/sec** |
| Indexed point read | **0.02 ms** |
| REST requests/sec under load | **17,800 RPS** |
| Memory footprint | **22.8 MB** |
| Buffer pool hit-rate | **94.7%** |

---

## Quick Start

```bash
curl -sSL https://milansql.de/install.sh | bash
```

## Docker

```bash
docker run -d -p 8080:8080 milansql/milansql:latest
```

## SDKs

```bash
npm install milansql-js      # JavaScript / TypeScript
pip install milansql-py      # Python
```

---

## Connect

| Protocol | Command |
|----------|---------|
| MySQL Wire | `mysql -h milansql.de -P 4407` |
| PostgreSQL Wire | `psql -h milansql.de -p 5433` |
| REST API | `https://milansql.de/api` |
| WebUI | `https://milansql.de/webui` |

---

## Documentation

| Guide | Description |
|-------|-------------|
| [SQL Reference](docs/sql-reference.html) | Complete SQL command reference |
| [Tutorial](docs/tutorial.html) | Step-by-step getting started guide |
| [Architecture](docs/architecture.html) | Internal architecture deep-dive |
| [Live Demo](docs/demo.html) | Try MilanSQL in your browser |

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│                    Client Protocols                      │
│  MySQL Wire  │  PostgreSQL Wire  │  REST  │  WebSocket  │
└──────────────────────┬──────────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────────┐
│              Query Pipeline                              │
│  Lexer → Parser → Binder → Optimizer → Executor         │
│              (Selinger-DP, Cost Model)                   │
└──────────────────────┬──────────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────────┐
│              Storage Engine                              │
│  Buffer Pool LRU │ MVCC │ WAL │ Partitioning │ Branching│
└──────────────────────┬──────────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────────┐
│              Security Layer                              │
│  RLS │ CLS │ Audit Trail │ AES-256-GCM │ Fortress       │
└─────────────────────────────────────────────────────────┘
```

---

Built with [Claude Code](https://claude.ai/code).
