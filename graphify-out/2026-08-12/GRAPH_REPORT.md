# Graph Report - milansql  (2026-08-12)

## Corpus Check
- 259 files · ~569,308 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 2114 nodes · 4915 edges · 116 communities (75 shown, 41 thin omitted)
- Extraction: 97% EXTRACTED · 3% INFERRED · 0% AMBIGUOUS · INFERRED: 158 edges (avg confidence: 0.79)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `371f104a`
- Run `git rev-parse HEAD` and compare to check if the graph is stale.
- Run `graphify update .` after code changes (no API cost).

## Community Hubs (Navigation)
- ParsedCommand
- Engine
- milansql_tests.cpp
- Column
- SelectItem
- string
- Table
- vector
- Parser
- ExplainRequest
- Row
- WhereCondition
- columns
- .resolveTableName
- PartitionInfo
- .insert
- BufferedOp
- .executeTriggerBody
- .Row
- engine.hpp
- TriggerDef
- .vacuum
- FullTextIndex
- .createPagedTable
- testGroup109
- .evalExprStr
- CursorData
- ExplainStep
- PartitionRangeDef
- .parseRlsExpr_
- SubscriptionDef
- PublicationDef
- .applyAndCommit
- .createMaterializedView
- .truncateTable
- .createColumnTable
- .setTableCompression
- ForeignTableDef
- .createRlsPolicy
- .getAllTableNamesInternal
- .getRlsPoliciesJsonForUser
- .getSchemaJson
- .loadRls
- pair
- unique_ptr
- unique_ptr
- vector
- Engine
- map
- function
- pair
- string
- vector
- map
- string
- string
- vector
- Parser
- .buildPgCatalogTable
- ExistsSpec
- MilanBinaryStorage
- sock_t
- .createColumnTable
- atomic
- pruneChildren
- atomic
- mutex
- ExplainPlan
- Table
- time_point
- ProcedureDef
- FetchDirection
- time_point
- HavingCondition
- IndexInfo
- JoinClause
- mutex
- ParsedCommand
- Parser
- ScalarSubSpec
- SelectItem
- Column
- Engine
- Row
- Column
- SelectItem
- Engine
- Table
- string
- Table
- unique_ptr
- vector
- WhereCondition
- extraction-spec.md
- pruneChildren
- function
- FetchDirection
- atomic
- mutex
- unique_ptr

## God Nodes (most connected - your core abstractions)
1. `Engine` - 461 edges
2. `ParsedCommand` - 263 edges
3. `Table` - 123 edges
4. `main()` - 118 edges
5. `check()` - 117 edges
6. `MilanHttpServer` - 75 edges
7. `execSQL()` - 61 edges
8. `columns` - 50 edges
9. `PgServer` - 47 edges
10. `Parser` - 45 edges

## Surprising Connections (you probably didn't know these)
- `testGroup96()` --calls--> `dispatch_slaveReadOnly()`  [INFERRED]
  src/tests/milansql_tests.cpp → src/dispatch.hpp
- `dispatch_projectExprs()` --references--> `ParsedCommand`  [INFERRED]
  src/dispatch.hpp → src/parser/parser.hpp
- `testGroup68()` --calls--> `dispatch_executeRecursiveCTE()`  [INFERRED]
  src/tests/milansql_tests.cpp → src/dispatch.hpp
- `handleAuthApiKeyCreate` --references--> `Parser`  [INFERRED]
  src/server/http_server.hpp → src/parser/parser.hpp
- `testGroup69()` --calls--> `valid`  [INFERRED]
  src/tests/milansql_tests.cpp → src/server/http_server.hpp

## Import Cycles
- None detected.

## Communities (116 total, 41 thin omitted)

### Community 0 - "ParsedCommand"
Cohesion: 0.01
Nodes (231): CommandType, FetchDirection, ForeignKeyDef, HavingCondition, JoinClause, ParsedPartitionList, ParsedPartitionRange, Column (+223 more)

### Community 1 - "Engine"
Cohesion: 0.01
Nodes (113): AccessControl, AuditLogger, BufferPool, CdcManager, CheckpointManager, ContinuousAggregateManager, DistributedLockManager, DistributedTxManager (+105 more)

### Community 2 - "milansql_tests.cpp"
Cohesion: 0.07
Nodes (129): run, bindParams(), cellVal(), check(), Engine, string, Table, vector (+121 more)

### Community 3 - "Column"
Cohesion: 0.07
Nodes (59): CursorSt, ExplainPlan, function, IndexInfo, Row, dispatch_applyVectorOrderBy(), dispatch_binlogWrite(), dispatch_buildCreateTableSql() (+51 more)

### Community 4 - "SelectItem"
Cohesion: 0.06
Nodes (32): SelectItem, againstQuery, aggCol, aggFunc, alias, caseElse, caseWhen, colName (+24 more)

### Community 5 - "string"
Cohesion: 0.16
Nodes (15): ifstream, ofstream, CopyManager, BINARY_MAGIC, lastStats_, CopyStats, durationMs, errors (+7 more)

### Community 6 - "Table"
Cohesion: 0.08
Nodes (10): DictCompressor, IndexEntry, size_t, Table, autoIncMap_, compressionType, dictCompressor, foreignKeys_ (+2 more)

### Community 7 - "vector"
Cohesion: 0.13
Nodes (11): Config, atomic, mutex, PitrManager, archivedSegments_, config_, lastArchiveTime_, mu_ (+3 more)

### Community 8 - "Parser"
Cohesion: 0.21
Nodes (6): ScalarSubSpec, pair, string, vector, Parser, WhereCondition

### Community 9 - "ExplainRequest"
Cohesion: 0.10
Nodes (20): ExplainRequest, aggCol, aggFunc, groupByCols, hasCaseItems, havingConds, isAggregate, isGroupBy (+12 more)

### Community 10 - "Row"
Cohesion: 0.13
Nodes (13): map, string, vector, MigrationDef, appliedAt, name, rollbackSql, sql (+5 more)

### Community 11 - "WhereCondition"
Cohesion: 0.06
Nodes (12): RwLockInfo, FetchDirection, pair, vector, IndexInfo, colName, indexName, type (+4 more)

### Community 12 - "columns"
Cohesion: 0.07
Nodes (26): For /graphify add and --watch, For /graphify query, For the commit hook and native CLAUDE.md integration, For --update and --cluster-only, /graphify, Honesty Rules, Interpreter guard for subcommands, Part A - Structural extraction for code files (+18 more)

### Community 14 - "PartitionInfo"
Cohesion: 0.09
Nodes (18): PartitionType, PartitionInfo, children, column, hashCount, lists, physical, ranges (+10 more)

### Community 15 - ".insert"
Cohesion: 0.06
Nodes (54): CertInfo, pair, appendLEI(), appendLES(), appendNullStr(), appendStr(), appendU16LE(), appendU32LE() (+46 more)

### Community 16 - "BufferedOp"
Cohesion: 0.14
Nodes (13): BufferedOp, alterColName, alterColNew, alterColType, alterOp, opType, setCol, setVal (+5 more)

### Community 17 - ".executeTriggerBody"
Cohesion: 0.12
Nodes (11): pid_t, atomic, atomic, ServerlessManager, coldStartMs_, enabled_, idleTimeoutSec_, lastActivityMs_ (+3 more)

### Community 18 - ".Row"
Cohesion: 0.15
Nodes (15): map, mutex, string, vector, ShardedTable, nodes, numShards, shardKey (+7 more)

### Community 19 - "engine.hpp"
Cohesion: 0.09
Nodes (21): set, CheckConstraint, op, val, FullTextIndex, avgDocLength, cols, forwardIndex (+13 more)

### Community 20 - "TriggerDef"
Cohesion: 0.22
Nodes (7): TriggerDef, body, event, granularity, name, tableName, timing

### Community 21 - ".vacuum"
Cohesion: 0.09
Nodes (12): CheckpointManager, autoCheckpointEnabled_, autoCheckpointInterval_, CHECKPOINT_FILE, checkpointCount_, DEFAULT_INTERVAL, lastCheckpointTime_, totalTx_ (+4 more)

### Community 22 - "FullTextIndex"
Cohesion: 0.12
Nodes (6): Any, QueryBuilder, Stage an INSERT — call .execute() to run it., Stage an UPDATE — call .execute() to run it., Stage a DELETE — call .execute() to run it., Fluent SQL query builder — mirroring the JS SDK API.

### Community 23 - ".createPagedTable"
Cohesion: 0.29
Nodes (7): vector, WalArchiveEntry, endLsn, filename, sizeBytes, startLsn, timestamp

### Community 24 - "testGroup109"
Cohesion: 0.05
Nodes (104): atomic, AuthManager, LockoutInfo, MilanBinaryStorage, mutex, RateLimiter, shared_mutex, sock_t (+96 more)

### Community 25 - ".evalExprStr"
Cohesion: 0.05
Nodes (30): RepairResult, columns, rows, HavingCondition, aggCol, aggFunc, op, val (+22 more)

### Community 26 - "CursorData"
Cohesion: 0.21
Nodes (12): BufferedOp, RecoveryResult, Engine, string, vector, WalRecovery, recover, WalTxEntry (+4 more)

### Community 27 - "ExplainStep"
Cohesion: 0.20
Nodes (9): OptimizerNote, ExplainPlan, steps, ExplainStep, details, index, nr, op (+1 more)

### Community 28 - "PartitionRangeDef"
Cohesion: 0.33
Nodes (5): ForeignKeyDef, fromCol, onDelete, refCol, refTable

### Community 29 - ".parseRlsExpr_"
Cohesion: 0.06
Nodes (35): array, LogLevel, map, atomic, mutex, string, StructuredLogger, json_format (+27 more)

### Community 30 - "SubscriptionDef"
Cohesion: 0.18
Nodes (12): BranchManager, branches_, mu_, snapshots_, map, mutex, string, vector (+4 more)

### Community 32 - "PublicationDef"
Cohesion: 0.11
Nodes (16): pg_sock_t, QueryResult, Engine, MilanBinaryStorage, mutex, Parser, string, vector (+8 more)

### Community 33 - ".applyAndCommit"
Cohesion: 0.06
Nodes (23): ColumnTable, Column, autoIncrement, checks, defaultValue, generatedExpr, hasDefault, isGenerated (+15 more)

### Community 38 - ".createColumnTable"
Cohesion: 0.12
Nodes (17): condition_variable, F, queue, Result, atomic, atomic, function, Row (+9 more)

### Community 41 - ".createRlsPolicy"
Cohesion: 0.20
Nodes (10): ScalarSubSpec, aggCol, aggFunc, conds, subTable, whereLogic, SubCond, col (+2 more)

### Community 42 - ".getAllTableNamesInternal"
Cohesion: 0.41
Nodes (11): baselineExists(), Engine, string, execSql(), insertSampleData(), jsonEscape(), loadBaseline(), main() (+3 more)

### Community 44 - ".getRlsPoliciesJsonForUser"
Cohesion: 0.12
Nodes (11): LRUIndex, LRUList, optional, string, normalizeSql(), UserQueryCache, enabled_, index_ (+3 more)

### Community 45 - ".getSchemaJson"
Cohesion: 0.20
Nodes (9): BackupLabel, backupDir, epochTime, sizeBytes, startLsn, tableCount, timestamp, version (+1 more)

### Community 50 - "unique_ptr"
Cohesion: 0.33
Nodes (6): RestoreResult, Engine, string, PitrManager::parseTxTimestamp(), PitrManager::restoreToPoint(), parseTxTimestamp

### Community 53 - "Engine"
Cohesion: 0.12
Nodes (9): commands, CONFIG_DIR, CONFIG_FILE, fs, http, https, os, path (+1 more)

### Community 55 - "function"
Cohesion: 0.12
Nodes (15): author, bin, milansql, description, engines, node, homepage, keywords (+7 more)

### Community 57 - "string"
Cohesion: 0.12
Nodes (15): author, description, exports, homepage, import, keywords, license, main (+7 more)

### Community 58 - "vector"
Cohesion: 0.29
Nodes (7): ExistsSpec, condLeft, condOp, condRight, subConds, subTable, subWhereLogic

### Community 59 - "map"
Cohesion: 0.33
Nodes (6): JoinClause, joinType, onLeft, onRight, table, tableAlias

### Community 62 - "string"
Cohesion: 0.26
Nodes (3): AsyncMilanSQL, QueryResult, Async MilanSQL HTTP client (requires aiohttp).      Examples:         db = Async

### Community 63 - "vector"
Cohesion: 0.40
Nodes (5): time_point, UserCacheEntry, cachedAt, result, tableName

### Community 64 - "Parser"
Cohesion: 0.18
Nodes (3): RealtimeBuilder, RealtimeSubscription, TxClient

### Community 65 - ".buildPgCatalogTable"
Cohesion: 0.48
Nodes (5): fail(), login(), pass(), query(), chaos_test.sh script

### Community 66 - "ExistsSpec"
Cohesion: 0.40
Nodes (5): SubscriptionDef, connection, enabled, name, publication

### Community 70 - "atomic"
Cohesion: 0.29
Nodes (10): dispatch(), evalScalarExpr(), string, vector, QueryResult, columns, error, message (+2 more)

### Community 71 - "pruneChildren"
Cohesion: 0.22
Nodes (5): milansql.http — HTTP fluent client for MilanSQL (Phase 4.4) Supabase-compatible, Query builder that records SQL for transaction replay., Transaction context for use with MilanSQL.transaction()., Transaction, TxQueryBuilder

### Community 75 - "ExplainPlan"
Cohesion: 0.22
Nodes (8): graphify reference: extra exports and benchmark, Step 6b - Wiki (only if --wiki flag), Step 7 - Neo4j export (only if --neo4j or --neo4j-push flag), Step 7a - FalkorDB export (only if --falkordb or --falkordb-push flag), Step 7b - SVG export (only if --svg flag), Step 7c - GraphML export (only if --graphml flag), Step 7d - MCP server (only if --mcp flag), Step 8 - Token reduction benchmark (only if total_words > 5000)

### Community 77 - "time_point"
Cohesion: 0.22
Nodes (8): connect(), connect_http(), milansql — Python client for MilanSQL database  TCP (DB-API 2.0):     import mil, Connect to MilanSQL via HTTP REST API with fluent query builder., Open a TCP connection to a MilanSQL server.      Accepts either a DSN string or, Open an HTTP connection to a MilanSQL REST API server.      Args:         host:, Connection, HttpConnection

### Community 78 - "ProcedureDef"
Cohesion: 0.44
Nodes (3): string, pitr_epoch_to_str(), pitr_now_epoch()

### Community 82 - "HavingCondition"
Cohesion: 0.33
Nodes (5): For /graphify explain, For /graphify path, graphify reference: query, path, explain, Step 0 — Constrained query expansion (REQUIRED before traversal), Step 1 — Traversal

### Community 84 - "IndexInfo"
Cohesion: 0.40
Nodes (5): BranchInfo, created_at, name, parent, status

### Community 85 - "JoinClause"
Cohesion: 0.40
Nodes (4): mutex, BufferedOp, Engine, pitr_str_to_epoch()

### Community 86 - "mutex"
Cohesion: 0.50
Nodes (3): For /graphify add, For --watch, graphify reference: add a URL and watch a folder

### Community 89 - "ScalarSubSpec"
Cohesion: 0.50
Nodes (3): For git commit hook, For native CLAUDE.md integration, graphify reference: commit hook and native CLAUDE.md integration

### Community 94 - "Column"
Cohesion: 0.50
Nodes (3): For --cluster-only, For --update (incremental re-extraction), graphify reference: incremental update and cluster-only

## Knowledge Gaps
- **722 isolated node(s):** `graphify`, `Usage`, `What graphify is for`, `Step 0 - GitHub repos and multi-path merge (only if a URL or several paths)`, `Step 1 - Ensure graphify is installed` (+717 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **41 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `Engine` connect `Engine` to `Table`, `WhereCondition`, `.resolveTableName`, `PartitionInfo`, `BufferedOp`, `.executeTriggerBody`, `engine.hpp`, `TriggerDef`, `.evalExprStr`, `ExplainStep`, `SubscriptionDef`, `.saveSubscriptions_`, `.applyAndCommit`, `.createMaterializedView`, `.createPublication`, `.getColumnStoreIndex`, `.setTableCompression`, `ForeignTableDef`, `.rollbackTransaction`, `.prepareTx`, `.printInheritanceTree_`, `vector`, `map`, `pair`, `vector`, `ExistsSpec`, `atomic`, `.insertPartitioned`, `.setTableAutoInc`, `JoinClause`, `Table`, `pruneChildren`?**
  _High betweenness centrality (0.283) - this node is a cross-community bridge._
- **Why does `ParsedCommand` connect `ParsedCommand` to `Parser`, `milansql_tests.cpp`, `Column`, `atomic`?**
  _High betweenness centrality (0.201) - this node is a cross-community bridge._
- **Why does `dispatch()` connect `atomic` to `ParsedCommand`, `.applyAndCommit`, `Engine`, `milansql_tests.cpp`, `Table`, `.getAllTableNamesInternal`, `WhereCondition`, `.evalExprStr`?**
  _High betweenness centrality (0.154) - this node is a cross-community bridge._
- **Are the 3 inferred relationships involving `ParsedCommand` (e.g. with `dispatch_projectExprs()` and `.splitTrim()`) actually correct?**
  _`ParsedCommand` has 3 INFERRED edges - model-reasoned connections that need verification._
- **Are the 28 inferred relationships involving `Table` (e.g. with `.applyOp()` and `.applyRls_()`) actually correct?**
  _`Table` has 28 INFERRED edges - model-reasoned connections that need verification._
- **What connects `graphify`, `Usage`, `What graphify is for` to the rest of the system?**
  _737 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `ParsedCommand` be split into smaller, more focused modules?**
  _Cohesion score 0.008658008658008658 - nodes in this community are weakly interconnected._