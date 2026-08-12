# Graph Report - milansql  (2026-08-11)

## Corpus Check
- 248 files · ~544,863 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 1709 nodes · 4263 edges · 78 communities (49 shown, 29 thin omitted)
- Extraction: 97% EXTRACTED · 3% INFERRED · 0% AMBIGUOUS · INFERRED: 132 edges (avg confidence: 0.8)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `ad568c6e`
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
- pruneChildren
- .insertPartitioned
- atomic
- mutex
- Table
- time_point
- ProcedureDef
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
- `testGroup69()` --calls--> `valid`  [INFERRED]
  src/tests/milansql_tests.cpp → src/server/http_server.hpp
- `main()` --calls--> `run`  [INFERRED]
  src/main.cpp → src/server/http_server.hpp
- `testGroup96()` --calls--> `dispatch_slaveReadOnly()`  [INFERRED]
  src/tests/milansql_tests.cpp → src/dispatch.hpp
- `testGroup68()` --calls--> `dispatch_executeRecursiveCTE()`  [INFERRED]
  src/tests/milansql_tests.cpp → src/dispatch.hpp
- `execSql()` --calls--> `dispatch()`  [INFERRED]
  src/tests/tpch_regression.cpp → src/dispatch_result.hpp

## Import Cycles
- None detected.

## Communities (78 total, 29 thin omitted)

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
Cohesion: 0.22
Nodes (26): ExplainPlan, ParsedCommand, Parser, SelectItem, dispatch_binlogWrite(), dispatch_buildCreateTableSql(), dispatch_displayVal(), dispatch_execSelectWithExprs() (+18 more)

### Community 4 - "SelectItem"
Cohesion: 0.04
Nodes (49): ExistsSpec, condLeft, condOp, condRight, subConds, subTable, subWhereLogic, ScalarSubSpec (+41 more)

### Community 5 - "string"
Cohesion: 0.16
Nodes (15): ifstream, ofstream, CopyManager, BINARY_MAGIC, lastStats_, CopyStats, durationMs, errors (+7 more)

### Community 6 - "Table"
Cohesion: 0.08
Nodes (11): DictCompressor, IndexEntry, size_t, Table, autoIncMap_, compressionType, dictCompressor, foreignKeys_ (+3 more)

### Community 7 - "vector"
Cohesion: 0.13
Nodes (11): Config, atomic, mutex, PitrManager, archivedSegments_, config_, lastArchiveTime_, mu_ (+3 more)

### Community 8 - "Parser"
Cohesion: 0.21
Nodes (6): ScalarSubSpec, pair, string, vector, Parser, WhereCondition

### Community 9 - "ExplainRequest"
Cohesion: 0.06
Nodes (31): ExplainRequest, aggCol, aggFunc, groupByCols, hasCaseItems, havingConds, isAggregate, isGroupBy (+23 more)

### Community 10 - "Row"
Cohesion: 0.38
Nodes (4): function, string, ProcExec, sanitizeForLog()

### Community 11 - "WhereCondition"
Cohesion: 0.08
Nodes (7): RwLockInfo, pair, vector, ProcedureDef, body, name, params

### Community 12 - "columns"
Cohesion: 0.07
Nodes (21): Row, values, xmax, xmin, WhereCondition, againstQuery, betweenHigh, betweenLow (+13 more)

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
Cohesion: 0.31
Nodes (5): string, vector, pitr_epoch_to_str(), pitr_now_epoch(), filename

### Community 19 - "engine.hpp"
Cohesion: 0.11
Nodes (17): map, CheckConstraint, op, val, IndexInfo, colName, indexName, type (+9 more)

### Community 20 - "TriggerDef"
Cohesion: 0.13
Nodes (8): PagedTable, TriggerDef, body, event, granularity, name, tableName, timing

### Community 21 - ".vacuum"
Cohesion: 0.09
Nodes (12): CheckpointManager, autoCheckpointEnabled_, autoCheckpointInterval_, CHECKPOINT_FILE, checkpointCount_, DEFAULT_INTERVAL, lastCheckpointTime_, totalTx_ (+4 more)

### Community 22 - "FullTextIndex"
Cohesion: 0.13
Nodes (14): set, FullTextIndex, avgDocLength, cols, forwardIndex, invertedIndex, name, rowWordCount (+6 more)

### Community 23 - ".createPagedTable"
Cohesion: 0.18
Nodes (10): atomic, mutex, BufferedOp, Engine, pitr_str_to_epoch(), WalArchiveEntry, endLsn, sizeBytes (+2 more)

### Community 24 - "testGroup109"
Cohesion: 0.06
Nodes (97): atomic, AuthManager, Engine, LockoutInfo, map, MilanBinaryStorage, mutex, RateLimiter (+89 more)

### Community 25 - ".evalExprStr"
Cohesion: 0.14
Nodes (3): columns, rows, TableCheckResult

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
Nodes (35): array, LogLevel, atomic, mutex, string, StructuredLogger, json_format, level (+27 more)

### Community 30 - "SubscriptionDef"
Cohesion: 0.31
Nodes (9): Column, IndexInfo, Row, dispatch_evalExprAtom(), dispatch_evalExprFull(), dispatch_printIndexes(), dispatch_projectExprs(), vector (+1 more)

### Community 32 - "PublicationDef"
Cohesion: 0.11
Nodes (16): pg_sock_t, QueryResult, Engine, MilanBinaryStorage, mutex, Parser, string, vector (+8 more)

### Community 33 - ".applyAndCommit"
Cohesion: 0.06
Nodes (23): ColumnTable, Column, autoIncrement, checks, defaultValue, generatedExpr, hasDefault, isGenerated (+15 more)

### Community 36 - ".truncateTable"
Cohesion: 0.19
Nodes (10): columns_, rows_, Engine, function, Parser, string, handleBackslashCommand(), main() (+2 more)

### Community 37 - ".getColumnStoreIndex"
Cohesion: 0.24
Nodes (3): OptimizerNote, ExplainPlan, steps

### Community 42 - ".getAllTableNamesInternal"
Cohesion: 0.41
Nodes (11): baselineExists(), Engine, string, execSql(), insertSampleData(), jsonEscape(), loadBaseline(), main() (+3 more)

### Community 44 - ".getRlsPoliciesJsonForUser"
Cohesion: 0.05
Nodes (34): condition_variable, F, LRUIndex, LRUList, optional, queue, Result, atomic (+26 more)

### Community 45 - ".getSchemaJson"
Cohesion: 0.22
Nodes (9): BackupLabel, backupDir, epochTime, sizeBytes, startLsn, tableCount, timestamp, version (+1 more)

### Community 46 - ".loadRls"
Cohesion: 0.29
Nodes (7): CursorSt, ProcState, cursors, hasNotFoundHandler, notFoundVal, notFoundVar, vars

### Community 50 - "unique_ptr"
Cohesion: 0.29
Nodes (6): RestoreResult, Engine, string, PitrManager::parseTxTimestamp(), PitrManager::restoreToPoint(), parseTxTimestamp

### Community 57 - "string"
Cohesion: 0.67
Nodes (4): pair, dispatch_applyVectorOrderBy(), dispatch_sortWithVector(), dispatch_splitUnionAll()

### Community 65 - ".buildPgCatalogTable"
Cohesion: 0.48
Nodes (5): fail(), login(), pass(), query(), chaos_test.sh script

## Knowledge Gaps
- **609 isolated node(s):** `method`, `path`, `query`, `headers`, `body` (+604 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **29 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `Engine` connect `Engine` to `Table`, `WhereCondition`, `columns`, `.resolveTableName`, `PartitionInfo`, `BufferedOp`, `engine.hpp`, `TriggerDef`, `.vacuum`, `FullTextIndex`, `.createPagedTable`, `.evalExprStr`, `.saveSubscriptions_`, `.applyAndCommit`, `.createMaterializedView`, `.createPublication`, `.getColumnStoreIndex`, `.createColumnTable`, `.setTableCompression`, `ForeignTableDef`, `.rollbackTransaction`, `.prepareTx`, `.printInheritanceTree_`, `pair`, `vector`, `function`, `pair`, `vector`, `vector`, `.insertPartitioned`, `.setTableAutoInc`?**
  _High betweenness centrality (0.344) - this node is a cross-community bridge._
- **Why does `ParsedCommand` connect `ParsedCommand` to `Parser`, `milansql_tests.cpp`, `SubscriptionDef`?**
  _High betweenness centrality (0.271) - this node is a cross-community bridge._
- **Why does `MilanHttpServer` connect `testGroup109` to `milansql_tests.cpp`?**
  _High betweenness centrality (0.057) - this node is a cross-community bridge._
- **Are the 2 inferred relationships involving `ParsedCommand` (e.g. with `.splitTrim()` and `.splitValues()`) actually correct?**
  _`ParsedCommand` has 2 INFERRED edges - model-reasoned connections that need verification._
- **Are the 28 inferred relationships involving `Table` (e.g. with `.applyOp()` and `.applyRls_()`) actually correct?**
  _`Table` has 28 INFERRED edges - model-reasoned connections that need verification._
- **What connects `method`, `path`, `query` to the rest of the system?**
  _609 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `ParsedCommand` be split into smaller, more focused modules?**
  _Cohesion score 0.008928571428571428 - nodes in this community are weakly interconnected._