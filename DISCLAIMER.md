# About MilanSQL

## Purpose
MilanSQL is an **educational project** built to deeply
understand how relational databases work internally.

## What this is
- A learning journey through database internals
- Implementation of concepts from PostgreSQL, MariaDB, SQLite
- Built with Claude Code as AI pair programmer
- 130 development phases, each teaching something new

## What this is NOT
- A production replacement for PostgreSQL or MySQL
- A fork or copy of existing database code
- A commercial product

## Concepts studied and implemented from scratch:
- B-Tree indexing (inspired by academic papers)
- MVCC (inspired by PostgreSQL internals documentation)
- WAL (inspired by SQLite WAL documentation)
- Hash Join / Merge Join (classic CS algorithms)
- BM25 (Okapi BM25 academic paper)
- HNSW (Malkov & Yashunin 2018 paper)
- DP Join Order (System R optimizer paper, 1979)

## Tools used
- Claude Code (Anthropic) as AI pair programmer
- C++17 standard library (zero external dependencies)
- CMake build system
- GitHub Actions CI/CD

## Learning outcomes
Understanding how databases work at the lowest level:
storage engines, query planners, transaction managers,
network protocols, and replication systems.
