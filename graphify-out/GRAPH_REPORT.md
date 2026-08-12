# Graph Report - milansql  (2026-08-12)

## Corpus Check
- 251 files · ~549,786 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 1794 nodes · 4724 edges · 103 communities (57 shown, 46 thin omitted)
- Extraction: 91% EXTRACTED · 9% INFERRED · 0% AMBIGUOUS · INFERRED: 426 edges (avg confidence: 0.8)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `d7dc3e5d`
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
- .insertPartitioned
- atomic
- mutex
- ExplainPlan
- Table
- time_point
- ProcedureDef
- FetchDirection
- time_point
- ForeignKeyDef
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

## God Nodes (most connected - your core abstractions)
1. `Engine` - 487 edges
2. `ParsedCommand` - 263 edges
3. `Table` - 148 edges
4. `main()` - 116 edges
5. `check()` - 115 edges
6. `execSQL()` - 72 edges
7. `MilanHttpServer` - 69 edges
8. `Column` - 61 edges
9. `executeSelect()` - 53 edges
10. `columns` - 50 edges

## Surprising Connections (you probably didn't know these)
- `testGroup69()` --calls--> `valid`  [INFERRED]
  src/tests/milansql_tests.cpp → src/server/http_server.hpp
- `sanitizeForLog()` --references--> `PartitionRangeDef`  [INFERRED]
  src/dispatch.hpp → src/engine/engine.hpp
- `testGroup96()` --calls--> `dispatch_slaveReadOnly()`  [INFERRED]
  src/tests/milansql_tests.cpp → src/dispatch.hpp
- `dispatch_applyVectorOrderBy()` --calls--> `rows_`  [INFERRED]
  src/dispatch.hpp → src/engine/engine.hpp
- `dispatch_printTable()` --calls--> `rows_`  [INFERRED]
  src/dispatch.hpp → src/engine/engine.hpp

## Import Cycles
- None detected.

## Communities (103 total, 46 thin omitted)

### Community 0 - "ParsedCommand"
Cohesion: 0.01
Nodes (226): CommandType, ParsedPartitionList, ParsedPartitionRange, FetchDirection, map, ParsedCommand, addListDef, addRangeDef (+218 more)

### Community 1 - "Engine"
Cohesion: 0.01
Nodes (113): AccessControl, AuditLogger, BufferPool, CdcManager, CheckpointManager, ContinuousAggregateManager, DistributedLockManager, DistributedTxManager (+105 more)

### Community 2 - "milansql_tests.cpp"
Cohesion: 0.05
Nodes (137): dispatch(), evalScalarExpr(), string, vector, QueryResult, columns, error, message (+129 more)

### Community 3 - "Column"
Cohesion: 0.06
Nodes (68): CursorSt, dispatch_applyVectorOrderBy(), dispatch_binlogWrite(), dispatch_buildCreateTableSql(), dispatch_displayVal(), dispatch_evalExprAtom(), dispatch_evalExprFull(), dispatch_execSelectWithExprs() (+60 more)

### Community 4 - "SelectItem"
Cohesion: 0.06
Nodes (32): SelectItem, againstQuery, aggCol, aggFunc, alias, caseElse, caseWhen, colName (+24 more)

### Community 5 - "string"
Cohesion: 0.22
Nodes (9): ifstream, ofstream, Row, CopyManager, BINARY_MAGIC, lastStats_, Engine, string (+1 more)

### Community 6 - "Table"
Cohesion: 0.09
Nodes (9): DictCompressor, IndexEntry, size_t, Table, autoIncMap_, compressionType, dictCompressor, foreignKeys_ (+1 more)

### Community 7 - "vector"
Cohesion: 0.14
Nodes (15): Config, atomic, mutex, string, pitr_epoch_to_str(), pitr_now_epoch(), PitrManager, archivedSegments_ (+7 more)

### Community 8 - "Parser"
Cohesion: 0.22
Nodes (4): pair, string, vector, Parser

### Community 9 - "ExplainRequest"
Cohesion: 0.09
Nodes (21): OptimizerNote, ExplainRequest, aggCol, aggFunc, groupByCols, hasCaseItems, havingConds, isAggregate (+13 more)

### Community 11 - "WhereCondition"
Cohesion: 0.05
Nodes (17): RwLockInfo, CursorData, currentPos, isOpen, name, sql, pair, vector (+9 more)

### Community 12 - "columns"
Cohesion: 0.12
Nodes (16): WhereCondition, againstQuery, betweenHigh, betweenLow, col, existsSpec, funcLhsExpr, inList (+8 more)

### Community 14 - "PartitionInfo"
Cohesion: 0.13
Nodes (15): PartitionType, PartitionInfo, children, column, hashCount, lists, physical, ranges (+7 more)

### Community 15 - ".insert"
Cohesion: 0.06
Nodes (54): CertInfo, pair, appendLEI(), appendLES(), appendNullStr(), appendStr(), appendU16LE(), appendU32LE() (+46 more)

### Community 16 - "BufferedOp"
Cohesion: 0.14
Nodes (13): BufferedOp, alterColName, alterColNew, alterColType, alterOp, opType, setCol, setVal (+5 more)

### Community 17 - ".executeTriggerBody"
Cohesion: 0.12
Nodes (10): pid_t, atomic, ServerlessManager, coldStartMs_, enabled_, idleTimeoutSec_, lastActivityMs_, suspended_ (+2 more)

### Community 18 - ".Row"
Cohesion: 0.15
Nodes (15): map, mutex, string, vector, ShardedTable, nodes, numShards, shardKey (+7 more)

### Community 19 - "engine.hpp"
Cohesion: 0.12
Nodes (16): set, CheckConstraint, op, val, IndexInfo, colName, indexName, type (+8 more)

### Community 20 - "TriggerDef"
Cohesion: 0.25
Nodes (7): TriggerDef, body, event, granularity, name, tableName, timing

### Community 21 - ".vacuum"
Cohesion: 0.09
Nodes (12): CheckpointManager, autoCheckpointEnabled_, autoCheckpointInterval_, CHECKPOINT_FILE, checkpointCount_, DEFAULT_INTERVAL, lastCheckpointTime_, totalTx_ (+4 more)

### Community 22 - "FullTextIndex"
Cohesion: 0.22
Nodes (9): FullTextIndex, avgDocLength, cols, forwardIndex, invertedIndex, name, rowWordCount, tableName (+1 more)

### Community 23 - ".createPagedTable"
Cohesion: 0.18
Nodes (10): atomic, mutex, BufferedOp, Engine, pitr_str_to_epoch(), WalArchiveEntry, endLsn, sizeBytes (+2 more)

### Community 24 - "testGroup109"
Cohesion: 0.06
Nodes (96): AuthManager, LockoutInfo, MilanBinaryStorage, RateLimiter, shared_mutex, sock_t, appendUtf8(), bindParams() (+88 more)

### Community 25 - ".evalExprStr"
Cohesion: 0.11
Nodes (4): columns, rows, pruneChildren, TableCheckResult

### Community 26 - "CursorData"
Cohesion: 0.21
Nodes (12): BufferedOp, RecoveryResult, Engine, string, vector, WalRecovery, recover, WalTxEntry (+4 more)

### Community 27 - "ExplainStep"
Cohesion: 0.33
Nodes (6): ExplainStep, details, index, nr, op, table

### Community 28 - "PartitionRangeDef"
Cohesion: 0.29
Nodes (5): ForeignKeyDef, fromCol, onDelete, refCol, refTable

### Community 29 - ".parseRlsExpr_"
Cohesion: 0.08
Nodes (21): array, atomic, mutex, time_point, MetricsCollector, buffer_hits, buffer_misses, connections_active (+13 more)

### Community 30 - "SubscriptionDef"
Cohesion: 0.13
Nodes (17): BranchInfo, created_at, name, parent, status, BranchManager, branches_, mu_ (+9 more)

### Community 32 - "PublicationDef"
Cohesion: 0.11
Nodes (16): pg_sock_t, QueryResult, Engine, MilanBinaryStorage, mutex, Parser, string, vector (+8 more)

### Community 33 - ".applyAndCommit"
Cohesion: 0.21
Nodes (4): PreparedStmt, name, paramCount, sql

### Community 36 - ".truncateTable"
Cohesion: 0.16
Nodes (14): LogLevel, map, atomic, mutex, string, StructuredLogger, json_format, level (+6 more)

### Community 38 - ".createColumnTable"
Cohesion: 0.15
Nodes (12): Result, atomic, function, Row, vector, parallelScan(), ThreadPool, cv_ (+4 more)

### Community 41 - ".createRlsPolicy"
Cohesion: 0.20
Nodes (10): ScalarSubSpec, aggCol, aggFunc, conds, subTable, whereLogic, SubCond, col (+2 more)

### Community 42 - ".getAllTableNamesInternal"
Cohesion: 0.36
Nodes (12): Engine, baselineExists(), Engine, string, execSql(), insertSampleData(), jsonEscape(), loadBaseline() (+4 more)

### Community 44 - ".getRlsPoliciesJsonForUser"
Cohesion: 0.12
Nodes (9): LRUIndex, LRUList, string, normalizeSql(), UserQueryCache, enabled_, index_, lru_ (+1 more)

### Community 45 - ".getSchemaJson"
Cohesion: 0.18
Nodes (10): BackupLabel, backupDir, epochTime, sizeBytes, startLsn, tableCount, timestamp, version (+2 more)

### Community 46 - ".loadRls"
Cohesion: 0.25
Nodes (7): condition_variable, F, optional, queue, atomic, mutex_, submit()

### Community 50 - "unique_ptr"
Cohesion: 0.29
Nodes (6): RestoreResult, Engine, string, PitrManager::parseTxTimestamp(), PitrManager::restoreToPoint(), parseTxTimestamp

### Community 53 - "Engine"
Cohesion: 0.25
Nodes (3): PartitionListDef, name, values

### Community 57 - "string"
Cohesion: 0.29
Nodes (7): CopyStats, durationMs, errors, fileName, lastOperation, rowsProcessed, tableName

### Community 58 - "vector"
Cohesion: 0.29
Nodes (7): ExistsSpec, condLeft, condOp, condRight, subConds, subTable, subWhereLogic

### Community 59 - "map"
Cohesion: 0.33
Nodes (6): JoinClause, joinType, onLeft, onRight, table, tableAlias

### Community 63 - "vector"
Cohesion: 0.40
Nodes (5): time_point, UserCacheEntry, cachedAt, result, tableName

### Community 64 - "Parser"
Cohesion: 0.40
Nodes (5): HavingCondition, aggCol, aggFunc, op, val

### Community 65 - ".buildPgCatalogTable"
Cohesion: 0.48
Nodes (5): fail(), login(), pass(), query(), chaos_test.sh script

### Community 66 - "ExistsSpec"
Cohesion: 0.40
Nodes (5): SubscriptionDef, connection, enabled, name, publication

## Knowledge Gaps
- **636 isolated node(s):** `name`, `parent`, `created_at`, `status`, `tableName` (+631 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **46 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `Engine` connect `Engine` to `milansql_tests.cpp`, `Column`, `Table`, `ExplainRequest`, `WhereCondition`, `.resolveTableName`, `PartitionInfo`, `BufferedOp`, `engine.hpp`, `TriggerDef`, `FullTextIndex`, `.createPagedTable`, `testGroup109`, `.evalExprStr`, `PartitionRangeDef`, `SubscriptionDef`, `.saveSubscriptions_`, `.applyAndCommit`, `.createMaterializedView`, `.createPublication`, `.getColumnStoreIndex`, `.setTableCompression`, `ForeignTableDef`, `.rollbackTransaction`, `.prepareTx`, `.printInheritanceTree_`, `pair`, `vector`, `Engine`, `map`, `function`, `pair`, `vector`, `ExistsSpec`, `.createColumnTable`, `.insertPartitioned`, `.setTableAutoInc`?**
  _High betweenness centrality (0.354) - this node is a cross-community bridge._
- **Why does `ParsedCommand` connect `ParsedCommand` to `Parser`, `milansql_tests.cpp`, `Column`, `SelectItem`, `Parser`, `columns`, `map`, `PartitionRangeDef`, `SubscriptionDef`?**
  _High betweenness centrality (0.188) - this node is a cross-community bridge._
- **Why does `Table` connect `Table` to `Engine`, `milansql_tests.cpp`, `Column`, `WhereCondition`, `.resolveTableName`, `PartitionInfo`, `engine.hpp`, `FullTextIndex`, `testGroup109`, `.evalExprStr`, `PartitionRangeDef`, `.applyAndCommit`, `.createMaterializedView`, `.getColumnStoreIndex`, `.setTableCompression`, `pair`, `Engine`, `pair`, `.insertPartitioned`, `.setTableAutoInc`?**
  _High betweenness centrality (0.055) - this node is a cross-community bridge._
- **Are the 13 inferred relationships involving `Engine` (e.g. with `testGroup108()` and `testGroup109()`) actually correct?**
  _`Engine` has 13 INFERRED edges - model-reasoned connections that need verification._
- **Are the 3 inferred relationships involving `ParsedCommand` (e.g. with `dispatch_projectExprs()` and `.splitTrim()`) actually correct?**
  _`ParsedCommand` has 3 INFERRED edges - model-reasoned connections that need verification._
- **Are the 38 inferred relationships involving `Table` (e.g. with `dispatchCommand()` and `.subst()`) actually correct?**
  _`Table` has 38 INFERRED edges - model-reasoned connections that need verification._
- **What connects `name`, `parent`, `created_at` to the rest of the system?**
  _636 weakly-connected nodes found - possible documentation gaps or missing edges._