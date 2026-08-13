# Graph Report - milansql  (2026-07-24)

## Corpus Check
- 244 files · ~536,762 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 1602 nodes · 3980 edges · 78 communities (50 shown, 28 thin omitted)
- Extraction: 99% EXTRACTED · 1% INFERRED · 0% AMBIGUOUS · INFERRED: 59 edges (avg confidence: 0.8)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `f91ff480`
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
- CursorData
- ExplainStep
- PartitionRangeDef
- .parseRlsExpr_
- SubscriptionDef
- PublicationDef
- .applyAndCommit
- .createMaterializedView
- .truncateTable
- .getColumnStoreIndex
- .createColumnTable
- .setTableCompression
- ForeignTableDef
- .createRlsPolicy
- .getRlsPoliciesJsonForUser
- .getSchemaJson
- .loadRls
- .prepareTx
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
- vector
- string
- vector
- Parser
- ExistsSpec
- MilanBinaryStorage
- sock_t
- JoinClause
- HavingCondition
- pruneChildren
- atomic
- mutex
- map
- Table
- time_point

## God Nodes (most connected - your core abstractions)
1. `Engine` - 455 edges
2. `ParsedCommand` - 249 edges
3. `Table` - 123 edges
4. `main()` - 108 edges
5. `check()` - 107 edges
6. `MilanHttpServer` - 69 edges
7. `execSQL()` - 60 edges
8. `columns` - 50 edges
9. `PgServer` - 47 edges
10. `SelectItem` - 45 edges

## Surprising Connections (you probably didn't know these)
- `testGroup69()` --calls--> `valid`  [INFERRED]
  src/tests/milansql_tests.cpp → src/server/http_server.hpp
- `main()` --calls--> `run`  [INFERRED]
  src/main.cpp → src/server/http_server.hpp
- `main()` --calls--> `columns_`  [INFERRED]
  src/main.cpp → src/engine/engine.hpp
- `main()` --calls--> `rows_`  [INFERRED]
  src/main.cpp → src/engine/engine.hpp
- `main()` --calls--> `recover`  [INFERRED]
  src/main.cpp → src/wal/wal_recovery.hpp

## Import Cycles
- None detected.

## Communities (78 total, 28 thin omitted)

### Community 0 - "ParsedCommand"
Cohesion: 0.01
Nodes (223): CommandType, FetchDirection, ForeignKeyDef, HavingCondition, JoinClause, ParsedPartitionList, ParsedPartitionRange, map (+215 more)

### Community 1 - "Engine"
Cohesion: 0.01
Nodes (112): AccessControl, AuditLogger, BufferPool, CdcManager, ContinuousAggregateManager, DistributedLockManager, DistributedTxManager, ExtensionManager (+104 more)

### Community 2 - "milansql_tests.cpp"
Cohesion: 0.07
Nodes (119): run, bindParams(), cellVal(), check(), Engine, string, vector, execSQL() (+111 more)

### Community 3 - "Column"
Cohesion: 0.10
Nodes (50): Column, CursorSt, Engine, ExplainPlan, function, IndexInfo, pair, ParsedCommand (+42 more)

### Community 4 - "SelectItem"
Cohesion: 0.06
Nodes (32): SelectItem, againstQuery, aggCol, aggFunc, alias, caseElse, caseWhen, colName (+24 more)

### Community 6 - "Table"
Cohesion: 0.08
Nodes (11): DictCompressor, IndexEntry, size_t, Table, autoIncMap_, compressionType, dictCompressor, foreignKeys_ (+3 more)

### Community 7 - "vector"
Cohesion: 0.06
Nodes (41): Config, RestoreResult, atomic, mutex, BackupLabel, backupDir, epochTime, sizeBytes (+33 more)

### Community 8 - "Parser"
Cohesion: 0.21
Nodes (7): ScalarSubSpec, Column, pair, string, vector, Parser, WhereCondition

### Community 9 - "ExplainRequest"
Cohesion: 0.10
Nodes (20): ExplainRequest, aggCol, aggFunc, groupByCols, hasCaseItems, havingConds, isAggregate, isGroupBy (+12 more)

### Community 10 - "Row"
Cohesion: 0.12
Nodes (16): WhereCondition, againstQuery, betweenHigh, betweenLow, col, existsSpec, funcLhsExpr, inList (+8 more)

### Community 11 - "WhereCondition"
Cohesion: 0.06
Nodes (11): RwLockInfo, pair, vector, ProcedureDef, body, name, params, Row (+3 more)

### Community 12 - "columns"
Cohesion: 0.09
Nodes (5): RepairResult, columns, rows, TableCheckResult, WhereResult

### Community 14 - "PartitionInfo"
Cohesion: 0.09
Nodes (18): PartitionType, PartitionInfo, children, column, hashCount, lists, physical, ranges (+10 more)

### Community 15 - ".insert"
Cohesion: 0.06
Nodes (53): CertInfo, appendLEI(), appendLES(), appendNullStr(), appendStr(), appendU16LE(), appendU32LE(), appendU8() (+45 more)

### Community 16 - "BufferedOp"
Cohesion: 0.14
Nodes (13): BufferedOp, alterColName, alterColNew, alterColType, alterOp, opType, setCol, setVal (+5 more)

### Community 17 - ".executeTriggerBody"
Cohesion: 0.50
Nodes (4): IndexInfo, colName, indexName, type

### Community 18 - ".Row"
Cohesion: 0.07
Nodes (32): AuthManager, LockoutInfo, MilanBinaryStorage, RateLimiter, atomic, Engine, mutex, time_point (+24 more)

### Community 19 - "engine.hpp"
Cohesion: 0.21
Nodes (8): Engine, map, string, vector, CheckConstraint, op, val, PitrHookInit

### Community 20 - "TriggerDef"
Cohesion: 0.22
Nodes (7): TriggerDef, body, event, granularity, name, tableName, timing

### Community 21 - ".vacuum"
Cohesion: 0.10
Nodes (12): CheckpointManager, autoCheckpointEnabled_, autoCheckpointInterval_, CHECKPOINT_FILE, checkpointCount_, DEFAULT_INTERVAL, lastCheckpointTime_, totalTx_ (+4 more)

### Community 22 - "FullTextIndex"
Cohesion: 0.13
Nodes (14): set, FullTextIndex, avgDocLength, cols, forwardIndex, invertedIndex, name, rowWordCount (+6 more)

### Community 24 - "testGroup109"
Cohesion: 0.17
Nodes (30): string, isJsonNumber(), isNumericType(), jsonEscape(), jsonValue(), jsonValueTyped(), handleAdminQuota, handleAdminStats (+22 more)

### Community 26 - "CursorData"
Cohesion: 0.21
Nodes (12): BufferedOp, RecoveryResult, Engine, string, vector, WalRecovery, recover, WalTxEntry (+4 more)

### Community 27 - "ExplainStep"
Cohesion: 0.33
Nodes (6): ExplainStep, details, index, nr, op, table

### Community 28 - "PartitionRangeDef"
Cohesion: 0.33
Nodes (5): ForeignKeyDef, fromCol, onDelete, refCol, refTable

### Community 29 - ".parseRlsExpr_"
Cohesion: 0.08
Nodes (21): array, atomic, mutex, time_point, MetricsCollector, buffer_hits, buffer_misses, connections_active (+13 more)

### Community 30 - "SubscriptionDef"
Cohesion: 0.40
Nodes (5): SubscriptionDef, connection, enabled, name, publication

### Community 32 - "PublicationDef"
Cohesion: 0.11
Nodes (16): pg_sock_t, QueryResult, Engine, MilanBinaryStorage, mutex, Parser, string, vector (+8 more)

### Community 33 - ".applyAndCommit"
Cohesion: 0.07
Nodes (23): ColumnTable, Column, autoIncrement, checks, defaultValue, generatedExpr, hasDefault, isGenerated (+15 more)

### Community 36 - ".truncateTable"
Cohesion: 0.25
Nodes (9): columns_, rows_, Engine, function, Parser, string, handleBackslashCommand(), main() (+1 more)

### Community 37 - ".getColumnStoreIndex"
Cohesion: 0.24
Nodes (3): OptimizerNote, ExplainPlan, steps

### Community 39 - ".setTableCompression"
Cohesion: 0.50
Nodes (4): PublicationDef, allTables, name, tables

### Community 44 - ".getRlsPoliciesJsonForUser"
Cohesion: 0.15
Nodes (15): LogLevel, map, ofstream, atomic, mutex, string, StructuredLogger, json_format (+7 more)

### Community 47 - ".prepareTx"
Cohesion: 0.16
Nodes (19): shared_mutex, sock_t, appendUtf8(), bindParams(), buildHttpResponse(), extractParamsFromJson(), extractSqlFromJson(), getQueryParam() (+11 more)

### Community 55 - "function"
Cohesion: 0.13
Nodes (15): HttpRequest, body, headers, method, path, query, extractApiKey, extractBearerToken (+7 more)

### Community 56 - "pair"
Cohesion: 0.24
Nodes (12): dispatch(), evalScalarExpr(), Column, Engine, Row, string, vector, QueryResult (+4 more)

### Community 58 - "vector"
Cohesion: 0.20
Nodes (10): ScalarSubSpec, aggCol, aggFunc, conds, subTable, whereLogic, SubCond, col (+2 more)

### Community 66 - "ExistsSpec"
Cohesion: 0.29
Nodes (7): ExistsSpec, condLeft, condOp, condRight, subConds, subTable, subWhereLogic

### Community 69 - "JoinClause"
Cohesion: 0.33
Nodes (6): JoinClause, joinType, onLeft, onRight, table, tableAlias

### Community 70 - "HavingCondition"
Cohesion: 0.40
Nodes (5): HavingCondition, aggCol, aggFunc, op, val

## Knowledge Gaps
- **589 isolated node(s):** `level`, `json_format`, `log_file`, `log_mu`, `log_path` (+584 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **28 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `Engine` connect `Engine` to `string`, `Table`, `vector`, `WhereCondition`, `columns`, `.resolveTableName`, `PartitionInfo`, `BufferedOp`, `engine.hpp`, `TriggerDef`, `.vacuum`, `FullTextIndex`, `.createPagedTable`, `.evalExprStr`, `SubscriptionDef`, `.saveSubscriptions_`, `.applyAndCommit`, `.createMaterializedView`, `.createPublication`, `.getColumnStoreIndex`, `.createColumnTable`, `.setTableCompression`, `ForeignTableDef`, `.getAllTableNamesInternal`, `.rollbackTransaction`, `.getSchemaJson`, `.loadRls`, `.printInheritanceTree_`, `pair`, `vector`, `.buildPgCatalogTable`, `pruneChildren`, `.insertPartitioned`?**
  _High betweenness centrality (0.361) - this node is a cross-community bridge._
- **Why does `ParsedCommand` connect `ParsedCommand` to `pair`, `Parser`, `engine.hpp`?**
  _High betweenness centrality (0.293) - this node is a cross-community bridge._
- **Why does `SelectItem` connect `SelectItem` to `.createMaterializedView`, `ExplainRequest`, `WhereCondition`, `columns`, `engine.hpp`, `vector`?**
  _High betweenness centrality (0.058) - this node is a cross-community bridge._
- **Are the 2 inferred relationships involving `ParsedCommand` (e.g. with `.splitTrim()` and `.splitValues()`) actually correct?**
  _`ParsedCommand` has 2 INFERRED edges - model-reasoned connections that need verification._
- **Are the 28 inferred relationships involving `Table` (e.g. with `.applyOp()` and `.applyRls_()`) actually correct?**
  _`Table` has 28 INFERRED edges - model-reasoned connections that need verification._
- **What connects `level`, `json_format`, `log_file` to the rest of the system?**
  _589 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `ParsedCommand` be split into smaller, more focused modules?**
  _Cohesion score 0.008968609865470852 - nodes in this community are weakly interconnected._