# Graph Report - milansql  (2026-08-11)

## Corpus Check
- 248 files · ~544,863 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 1709 nodes · 4264 edges · 82 communities (54 shown, 28 thin omitted)
- Extraction: 97% EXTRACTED · 3% INFERRED · 0% AMBIGUOUS · INFERRED: 132 edges (avg confidence: 0.8)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `5c2c1465`
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
- .getColumnStoreIndex
- .createColumnTable
- .setTableCompression
- ForeignTableDef
- .createRlsPolicy
- .getAllTableNamesInternal
- .getRlsPoliciesJsonForUser
- .getSchemaJson
- .loadRls
- unique_ptr
- unique_ptr
- vector
- Engine
- map
- function
- pair
- vector
- map
- string
- Parser
- .buildPgCatalogTable
- ExistsSpec
- MilanBinaryStorage
- sock_t
- JoinClause
- HavingCondition
- pruneChildren
- .insertPartitioned
- atomic
- mutex
- map
- Table
- time_point
- ProcedureDef
- CheckConstraint
- time_point

## God Nodes (most connected - your core abstractions)
1. `Engine` - 455 edges
2. `ParsedCommand` - 249 edges
3. `Table` - 123 edges
4. `main()` - 112 edges
5. `check()` - 111 edges
6. `MilanHttpServer` - 69 edges
7. `execSQL()` - 61 edges
8. `columns` - 50 edges
9. `PgServer` - 47 edges
10. `SelectItem` - 45 edges

## Surprising Connections (you probably didn't know these)
- `testGroup96()` --calls--> `dispatch_slaveReadOnly()`  [INFERRED]
  src/tests/milansql_tests.cpp → src/dispatch.hpp
- `testGroup68()` --calls--> `dispatch_executeRecursiveCTE()`  [INFERRED]
  src/tests/milansql_tests.cpp → src/dispatch.hpp
- `testGroup69()` --calls--> `valid`  [INFERRED]
  src/tests/milansql_tests.cpp → src/server/http_server.hpp
- `main()` --calls--> `run`  [INFERRED]
  src/main.cpp → src/server/http_server.hpp
- `execSql()` --calls--> `dispatch()`  [INFERRED]
  src/tests/tpch_regression.cpp → src/dispatch_result.hpp

## Import Cycles
- None detected.

## Communities (82 total, 28 thin omitted)

### Community 0 - "ParsedCommand"
Cohesion: 0.01
Nodes (224): CommandType, FetchDirection, ForeignKeyDef, HavingCondition, JoinClause, ParsedPartitionList, ParsedPartitionRange, Column (+216 more)

### Community 1 - "Engine"
Cohesion: 0.01
Nodes (112): AccessControl, AuditLogger, BufferPool, CdcManager, ContinuousAggregateManager, DistributedLockManager, DistributedTxManager, ExtensionManager (+104 more)

### Community 2 - "milansql_tests.cpp"
Cohesion: 0.06
Nodes (136): dispatch(), evalScalarExpr(), Column, Engine, Row, string, vector, QueryResult (+128 more)

### Community 3 - "Column"
Cohesion: 0.10
Nodes (50): Column, CursorSt, ExplainPlan, function, IndexInfo, pair, ParsedCommand, Parser (+42 more)

### Community 4 - "SelectItem"
Cohesion: 0.06
Nodes (32): SelectItem, againstQuery, aggCol, aggFunc, alias, caseElse, caseWhen, colName (+24 more)

### Community 5 - "string"
Cohesion: 0.16
Nodes (15): ifstream, ofstream, CopyManager, BINARY_MAGIC, lastStats_, CopyStats, durationMs, errors (+7 more)

### Community 6 - "Table"
Cohesion: 0.06
Nodes (12): DictCompressor, IndexEntry, size_t, function, Table, autoIncMap_, compressionType, dictCompressor (+4 more)

### Community 7 - "vector"
Cohesion: 0.06
Nodes (40): Config, RestoreResult, mutex, BackupLabel, backupDir, epochTime, sizeBytes, startLsn (+32 more)

### Community 8 - "Parser"
Cohesion: 0.21
Nodes (6): ScalarSubSpec, pair, string, vector, Parser, WhereCondition

### Community 9 - "ExplainRequest"
Cohesion: 0.10
Nodes (20): ExplainRequest, aggCol, aggFunc, groupByCols, hasCaseItems, havingConds, isAggregate, isGroupBy (+12 more)

### Community 10 - "Row"
Cohesion: 0.12
Nodes (16): WhereCondition, againstQuery, betweenHigh, betweenLow, col, existsSpec, funcLhsExpr, inList (+8 more)

### Community 11 - "WhereCondition"
Cohesion: 0.07
Nodes (8): RwLockInfo, pair, vector, ProcedureDef, body, name, params, WhereResult

### Community 12 - "columns"
Cohesion: 0.12
Nodes (4): Row, values, xmax, xmin

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

### Community 19 - "engine.hpp"
Cohesion: 0.23
Nodes (6): map, CheckConstraint, op, val, atomic, PitrHookInit

### Community 20 - "TriggerDef"
Cohesion: 0.22
Nodes (7): TriggerDef, body, event, granularity, name, tableName, timing

### Community 21 - ".vacuum"
Cohesion: 0.09
Nodes (12): CheckpointManager, autoCheckpointEnabled_, autoCheckpointInterval_, CHECKPOINT_FILE, checkpointCount_, DEFAULT_INTERVAL, lastCheckpointTime_, totalTx_ (+4 more)

### Community 22 - "FullTextIndex"
Cohesion: 0.13
Nodes (14): set, FullTextIndex, avgDocLength, cols, forwardIndex, invertedIndex, name, rowWordCount (+6 more)

### Community 24 - "testGroup109"
Cohesion: 0.06
Nodes (97): atomic, AuthManager, LockoutInfo, MilanBinaryStorage, mutex, RateLimiter, shared_mutex, sock_t (+89 more)

### Community 25 - ".evalExprStr"
Cohesion: 0.12
Nodes (4): columns, rows, pruneChildren, TableCheckResult

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
Cohesion: 0.06
Nodes (35): array, LogLevel, map, atomic, mutex, string, StructuredLogger, json_format (+27 more)

### Community 30 - "SubscriptionDef"
Cohesion: 0.40
Nodes (5): SubscriptionDef, connection, enabled, name, publication

### Community 32 - "PublicationDef"
Cohesion: 0.11
Nodes (16): pg_sock_t, QueryResult, Engine, MilanBinaryStorage, mutex, Parser, string, vector (+8 more)

### Community 33 - ".applyAndCommit"
Cohesion: 0.06
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

### Community 42 - ".getAllTableNamesInternal"
Cohesion: 0.41
Nodes (11): baselineExists(), Engine, string, execSql(), insertSampleData(), jsonEscape(), loadBaseline(), main() (+3 more)

### Community 44 - ".getRlsPoliciesJsonForUser"
Cohesion: 0.11
Nodes (12): LRUIndex, LRUList, optional, atomic, string, normalizeSql(), UserQueryCache, enabled_ (+4 more)

### Community 45 - ".getSchemaJson"
Cohesion: 0.12
Nodes (17): condition_variable, F, queue, Result, atomic, function, Row, vector (+9 more)

### Community 46 - ".loadRls"
Cohesion: 0.40
Nodes (5): time_point, UserCacheEntry, cachedAt, result, tableName

### Community 58 - "vector"
Cohesion: 0.20
Nodes (10): ScalarSubSpec, aggCol, aggFunc, conds, subTable, whereLogic, SubCond, col (+2 more)

### Community 65 - ".buildPgCatalogTable"
Cohesion: 0.48
Nodes (5): fail(), login(), pass(), query(), chaos_test.sh script

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
- **609 isolated node(s):** `lastOperation`, `tableName`, `fileName`, `rowsProcessed`, `errors` (+604 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **28 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `Engine` connect `Engine` to `Table`, `vector`, `WhereCondition`, `columns`, `.resolveTableName`, `PartitionInfo`, `BufferedOp`, `engine.hpp`, `TriggerDef`, `.vacuum`, `FullTextIndex`, `.createPagedTable`, `.evalExprStr`, `SubscriptionDef`, `.saveSubscriptions_`, `.applyAndCommit`, `.createMaterializedView`, `.createPublication`, `.getColumnStoreIndex`, `.createColumnTable`, `.setTableCompression`, `ForeignTableDef`, `.rollbackTransaction`, `.prepareTx`, `.printInheritanceTree_`, `pair`, `vector`, `function`, `pair`, `string`, `vector`, `string`, `vector`, `.insertPartitioned`, `.setTableAutoInc`?**
  _High betweenness centrality (0.347) - this node is a cross-community bridge._
- **Why does `ParsedCommand` connect `ParsedCommand` to `Parser`, `milansql_tests.cpp`, `Column`?**
  _High betweenness centrality (0.264) - this node is a cross-community bridge._
- **Why does `PgServer` connect `PublicationDef` to `.insert`?**
  _High betweenness centrality (0.056) - this node is a cross-community bridge._
- **Are the 2 inferred relationships involving `ParsedCommand` (e.g. with `.splitTrim()` and `.splitValues()`) actually correct?**
  _`ParsedCommand` has 2 INFERRED edges - model-reasoned connections that need verification._
- **Are the 28 inferred relationships involving `Table` (e.g. with `.applyOp()` and `.applyRls_()`) actually correct?**
  _`Table` has 28 INFERRED edges - model-reasoned connections that need verification._
- **What connects `lastOperation`, `tableName`, `fileName` to the rest of the system?**
  _609 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `ParsedCommand` be split into smaller, more focused modules?**
  _Cohesion score 0.008928571428571428 - nodes in this community are weakly interconnected._