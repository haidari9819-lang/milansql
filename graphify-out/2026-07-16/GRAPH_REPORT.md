# Graph Report - milansql  (2026-07-16)

## Corpus Check
- 240 files · ~528,925 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 1435 nodes · 3736 edges · 64 communities (40 shown, 24 thin omitted)
- Extraction: 98% EXTRACTED · 2% INFERRED · 0% AMBIGUOUS · INFERRED: 60 edges (avg confidence: 0.8)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `ee16e045`
- Run `git rev-parse HEAD` and compare to check if the graph is stale.
- Run `graphify update .` after code changes (no API cost).

## Community Hubs (Navigation)
- ParsedCommand
- Engine
- milansql_tests.cpp
- Column
- SelectItem
- Table
- vector
- Parser
- ExplainRequest
- Row
- WhereCondition
- .resolveTableName
- PartitionInfo
- .insert
- BufferedOp
- .executeTriggerBody
- .Row
- engine.hpp
- TriggerDef
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
- .truncateTable
- .getColumnStoreIndex
- .createColumnTable
- .setTableCompression
- ForeignTableDef
- .createRlsPolicy
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
- vector
- string
- vector

## God Nodes (most connected - your core abstractions)
1. `Engine` - 457 edges
2. `ParsedCommand` - 249 edges
3. `Table` - 123 edges
4. `main()` - 105 edges
5. `check()` - 104 edges
6. `MilanHttpServer` - 69 edges
7. `execSQL()` - 60 edges
8. `columns` - 50 edges
9. `PgServer` - 47 edges
10. `WhereCondition` - 47 edges

## Surprising Connections (you probably didn't know these)
- `main()` --calls--> `run`  [INFERRED]
  src/main.cpp → src/server/http_server.hpp
- `testGroup69()` --calls--> `valid`  [INFERRED]
  src/tests/milansql_tests.cpp → src/server/http_server.hpp
- `testGroup105()` --references--> `TlsContext`  [INFERRED]
  src/tests/milansql_tests.cpp → src/ssl/tls_context.hpp
- `testGroup110()` --calls--> `lastError_`  [INFERRED]
  src/tests/milansql_tests.cpp → src/ssl/tls_context.hpp
- `dispatch()` --calls--> `name_`  [INFERRED]
  src/dispatch_result.hpp → src/engine/engine.hpp

## Import Cycles
- None detected.

## Communities (64 total, 24 thin omitted)

### Community 0 - "ParsedCommand"
Cohesion: 0.01
Nodes (219): CommandType, ParsedPartitionList, ParsedPartitionRange, FetchDirection, map, ParsedCommand, addListDef, addRangeDef (+211 more)

### Community 1 - "Engine"
Cohesion: 0.01
Nodes (115): AccessControl, AuditLogger, BufferPool, CdcManager, CheckpointManager, ContinuousAggregateManager, DistributedLockManager, DistributedTxManager (+107 more)

### Community 2 - "milansql_tests.cpp"
Cohesion: 0.07
Nodes (116): bindParams(), cellVal(), check(), Engine, Parser, string, Table, vector (+108 more)

### Community 3 - "Column"
Cohesion: 0.10
Nodes (51): Column, CursorSt, ExplainPlan, function, IndexInfo, pair, ParsedCommand, Parser (+43 more)

### Community 4 - "SelectItem"
Cohesion: 0.04
Nodes (49): ExistsSpec, condLeft, condOp, condRight, subConds, subTable, subWhereLogic, ScalarSubSpec (+41 more)

### Community 6 - "Table"
Cohesion: 0.09
Nodes (13): CompressionType, DictCompressor, IndexEntry, size_t, Table, autoIncMap_, columns_, compressionType (+5 more)

### Community 8 - "Parser"
Cohesion: 0.24
Nodes (4): pair, string, vector, Parser

### Community 9 - "ExplainRequest"
Cohesion: 0.10
Nodes (20): ExplainRequest, aggCol, aggFunc, groupByCols, hasCaseItems, havingConds, isAggregate, isGroupBy (+12 more)

### Community 10 - "Row"
Cohesion: 0.16
Nodes (3): columns, rows, TableCheckResult

### Community 11 - "WhereCondition"
Cohesion: 0.09
Nodes (18): pruneChildren, WhereCondition, againstQuery, betweenHigh, betweenLow, col, existsSpec, funcLhsExpr (+10 more)

### Community 14 - "PartitionInfo"
Cohesion: 0.09
Nodes (18): PartitionType, PartitionInfo, children, column, hashCount, lists, physical, ranges (+10 more)

### Community 15 - ".insert"
Cohesion: 0.06
Nodes (54): CertInfo, appendLEI(), appendLES(), appendNullStr(), appendStr(), appendU16LE(), appendU32LE(), appendU8() (+46 more)

### Community 16 - "BufferedOp"
Cohesion: 0.14
Nodes (13): BufferedOp, alterColName, alterColNew, alterColType, alterOp, opType, setCol, setVal (+5 more)

### Community 17 - ".executeTriggerBody"
Cohesion: 0.07
Nodes (22): Column, autoIncrement, checks, defaultValue, generatedExpr, hasDefault, isGenerated, isPrimaryKey (+14 more)

### Community 18 - ".Row"
Cohesion: 0.05
Nodes (98): AuthManager, LockoutInfo, map, RateLimiter, shared_mutex, appendUtf8(), bindParams(), buildHttpResponse() (+90 more)

### Community 19 - "engine.hpp"
Cohesion: 0.22
Nodes (9): set, CheckConstraint, op, val, map, UserDef, grants, name (+1 more)

### Community 20 - "TriggerDef"
Cohesion: 0.22
Nodes (7): TriggerDef, body, event, granularity, name, tableName, timing

### Community 22 - "FullTextIndex"
Cohesion: 0.25
Nodes (8): FullTextIndex, avgDocLength, cols, forwardIndex, invertedIndex, name, rowWordCount, tableName

### Community 24 - "testGroup109"
Cohesion: 0.33
Nodes (6): JoinClause, joinType, onLeft, onRight, table, tableAlias

### Community 26 - "CursorData"
Cohesion: 0.17
Nodes (14): dispatch(), evalScalarExpr(), string, vector, QueryResult, columns, error, message (+6 more)

### Community 27 - "ExplainStep"
Cohesion: 0.20
Nodes (9): OptimizerNote, ExplainPlan, steps, ExplainStep, details, index, nr, op (+1 more)

### Community 28 - "PartitionRangeDef"
Cohesion: 0.33
Nodes (5): ForeignKeyDef, fromCol, onDelete, refCol, refTable

### Community 29 - ".parseRlsExpr_"
Cohesion: 0.40
Nodes (5): HavingCondition, aggCol, aggFunc, op, val

### Community 30 - "SubscriptionDef"
Cohesion: 0.40
Nodes (5): SubscriptionDef, connection, enabled, name, publication

### Community 32 - "PublicationDef"
Cohesion: 0.09
Nodes (22): pg_sock_t, QueryResult, Engine, Parser, string, handleBackslashCommand(), main(), printBanner() (+14 more)

### Community 36 - ".truncateTable"
Cohesion: 0.33
Nodes (4): ProcedureDef, body, name, params

### Community 37 - ".getColumnStoreIndex"
Cohesion: 0.40
Nodes (4): PreparedStmt, name, paramCount, sql

### Community 39 - ".setTableCompression"
Cohesion: 0.50
Nodes (4): PublicationDef, allTables, name, tables

## Knowledge Gaps
- **530 isolated node(s):** `port_`, `dbPath_`, `engine_`, `storage_`, `engineMutex_` (+525 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **24 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `Engine` connect `Engine` to `Column`, `string`, `Table`, `vector`, `Row`, `WhereCondition`, `columns`, `.resolveTableName`, `PartitionInfo`, `BufferedOp`, `.executeTriggerBody`, `engine.hpp`, `TriggerDef`, `.vacuum`, `FullTextIndex`, `.createPagedTable`, `.evalExprStr`, `CursorData`, `ExplainStep`, `SubscriptionDef`, `.saveSubscriptions_`, `.applyAndCommit`, `.createMaterializedView`, `.createPublication`, `.truncateTable`, `.getColumnStoreIndex`, `.createColumnTable`, `.setTableCompression`, `ForeignTableDef`, `.getAllTableNamesInternal`, `.rollbackTransaction`, `.getRlsPoliciesJsonForUser`, `.loadRls`, `.prepareTx`, `.printInheritanceTree_`, `unique_ptr`, `vector`, `Engine`?**
  _High betweenness centrality (0.344) - this node is a cross-community bridge._
- **Why does `ParsedCommand` connect `ParsedCommand` to `Column`, `SelectItem`, `Parser`, `WhereCondition`, `.executeTriggerBody`, `testGroup109`, `CursorData`, `PartitionRangeDef`, `.parseRlsExpr_`?**
  _High betweenness centrality (0.321) - this node is a cross-community bridge._
- **Why does `isNumericType()` connect `.Row` to `.executeTriggerBody`?**
  _High betweenness centrality (0.128) - this node is a cross-community bridge._
- **Are the 2 inferred relationships involving `ParsedCommand` (e.g. with `.splitTrim()` and `.splitValues()`) actually correct?**
  _`ParsedCommand` has 2 INFERRED edges - model-reasoned connections that need verification._
- **Are the 28 inferred relationships involving `Table` (e.g. with `.applyOp()` and `.applyRls_()`) actually correct?**
  _`Table` has 28 INFERRED edges - model-reasoned connections that need verification._
- **What connects `port_`, `dbPath_`, `engine_` to the rest of the system?**
  _530 weakly-connected nodes found - possible documentation gaps or missing edges._
- **Should `ParsedCommand` be split into smaller, more focused modules?**
  _Cohesion score 0.0091324200913242 - nodes in this community are weakly interconnected._