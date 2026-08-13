# Graph Report - milansql  (2026-07-15)

## Corpus Check
- 240 files · ~526,135 words
- Verdict: corpus is large enough that graph structure adds value.

## Summary
- 1281 nodes · 3594 edges · 59 communities (41 shown, 18 thin omitted)
- Extraction: 91% EXTRACTED · 9% INFERRED · 0% AMBIGUOUS · INFERRED: 309 edges (avg confidence: 0.8)
- Token cost: 0 input · 0 output

## Graph Freshness
- Built from commit: `0b1f75cd`
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
- ForeignTableDef
- .createRlsPolicy
- .getSchemaJson
- .loadRls
- pair
- unique_ptr
- Engine
- map
- function
- pair
- string
- vector

## God Nodes (most connected - your core abstractions)
1. `Engine` - 473 edges
2. `ParsedCommand` - 249 edges
3. `Table` - 130 edges
4. `main()` - 104 edges
5. `check()` - 103 edges
6. `execSQL()` - 72 edges
7. `MilanHttpServer` - 69 edges
8. `executeSelect()` - 53 edges
9. `columns` - 50 edges
10. `Column` - 49 edges

## Surprising Connections (you probably didn't know these)
- `dispatch()` --calls--> `name_`  [INFERRED]
  src/dispatch_result.hpp → src/engine/engine.hpp
- `testGroup96()` --calls--> `dispatch_slaveReadOnly()`  [INFERRED]
  src/tests/milansql_tests.cpp → src/dispatch.hpp
- `testGroup68()` --calls--> `dispatch_executeRecursiveCTE()`  [INFERRED]
  src/tests/milansql_tests.cpp → src/dispatch.hpp
- `cellVal()` --references--> `Column`  [INFERRED]
  src/tests/milansql_tests.cpp → src/engine/engine.hpp
- `executeSelect()` --references--> `Column`  [INFERRED]
  src/tests/milansql_tests.cpp → src/engine/engine.hpp

## Import Cycles
- None detected.

## Communities (59 total, 18 thin omitted)

### Community 0 - "ParsedCommand"
Cohesion: 0.01
Nodes (219): CommandType, ParsedPartitionList, ParsedPartitionRange, FetchDirection, map, ParsedCommand, addListDef, addRangeDef (+211 more)

### Community 1 - "Engine"
Cohesion: 0.01
Nodes (115): AccessControl, AuditLogger, BufferPool, CdcManager, CheckpointManager, ContinuousAggregateManager, DistributedLockManager, DistributedTxManager (+107 more)

### Community 2 - "milansql_tests.cpp"
Cohesion: 0.07
Nodes (121): dispatch(), evalScalarExpr(), string, vector, QueryResult, columns, error, message (+113 more)

### Community 3 - "Column"
Cohesion: 0.10
Nodes (51): Column, CursorSt, ExplainPlan, function, IndexInfo, pair, ParsedCommand, Parser (+43 more)

### Community 4 - "SelectItem"
Cohesion: 0.04
Nodes (49): ExistsSpec, condLeft, condOp, condRight, subConds, subTable, subWhereLogic, ScalarSubSpec (+41 more)

### Community 6 - "Table"
Cohesion: 0.11
Nodes (11): DictCompressor, IndexEntry, size_t, Table, autoIncMap_, compressionType, dictCompressor, foreignKeys_ (+3 more)

### Community 8 - "Parser"
Cohesion: 0.22
Nodes (4): pair, string, vector, Parser

### Community 9 - "ExplainRequest"
Cohesion: 0.10
Nodes (20): ExplainRequest, aggCol, aggFunc, groupByCols, hasCaseItems, havingConds, isAggregate, isGroupBy (+12 more)

### Community 10 - "Row"
Cohesion: 0.18
Nodes (4): Row, values, xmax, xmin

### Community 11 - "WhereCondition"
Cohesion: 0.09
Nodes (18): pruneChildren, WhereCondition, againstQuery, betweenHigh, betweenLow, col, existsSpec, funcLhsExpr (+10 more)

### Community 12 - "columns"
Cohesion: 0.15
Nodes (3): columns, rows, TableCheckResult

### Community 13 - ".resolveTableName"
Cohesion: 0.06
Nodes (5): ColumnStoreIndex, ColumnTable, CompressionType, RlsPolicy, testGroup109()

### Community 14 - "PartitionInfo"
Cohesion: 0.09
Nodes (18): PartitionType, PartitionInfo, children, column, hashCount, lists, physical, ranges (+10 more)

### Community 16 - "BufferedOp"
Cohesion: 0.14
Nodes (13): BufferedOp, alterColName, alterColNew, alterColType, alterOp, opType, setCol, setVal (+5 more)

### Community 17 - ".executeTriggerBody"
Cohesion: 0.22
Nodes (4): PreparedStmt, name, paramCount, sql

### Community 18 - ".Row"
Cohesion: 0.06
Nodes (98): atomic, AuthManager, LockoutInfo, MilanBinaryStorage, mutex, RateLimiter, shared_mutex, sock_t (+90 more)

### Community 19 - "engine.hpp"
Cohesion: 0.32
Nodes (6): set, map, UserDef, grants, name, passwordHash

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
Cohesion: 0.05
Nodes (28): FetchDirection, CheckConstraint, op, val, Column, autoIncrement, checks, defaultValue (+20 more)

### Community 27 - "ExplainStep"
Cohesion: 0.20
Nodes (9): OptimizerNote, ExplainPlan, steps, ExplainStep, details, index, nr, op (+1 more)

### Community 28 - "PartitionRangeDef"
Cohesion: 0.29
Nodes (5): ForeignKeyDef, fromCol, onDelete, refCol, refTable

### Community 29 - ".parseRlsExpr_"
Cohesion: 0.27
Nodes (5): HavingCondition, aggCol, aggFunc, op, val

### Community 30 - "SubscriptionDef"
Cohesion: 0.40
Nodes (5): SubscriptionDef, connection, enabled, name, publication

### Community 32 - "PublicationDef"
Cohesion: 0.50
Nodes (4): PublicationDef, allTables, name, tables

### Community 36 - ".truncateTable"
Cohesion: 0.33
Nodes (4): ProcedureDef, body, name, params

## Knowledge Gaps
- **503 isolated node(s):** `vars`, `cursors`, `notFoundVar`, `notFoundVal`, `hasNotFoundHandler` (+498 more)
  These have ≤1 connection - possible missing edges or undocumented components.
- **18 thin communities (<3 nodes) omitted from report** — run `graphify query` to explore isolated nodes.

## Suggested Questions
_Questions this graph is uniquely positioned to answer:_

- **Why does `Engine` connect `Engine` to `milansql_tests.cpp`, `Column`, `string`, `Table`, `vector`, `Row`, `WhereCondition`, `columns`, `.resolveTableName`, `PartitionInfo`, `.insert`, `BufferedOp`, `.executeTriggerBody`, `engine.hpp`, `TriggerDef`, `.vacuum`, `FullTextIndex`, `.createPagedTable`, `.evalExprStr`, `CursorData`, `ExplainStep`, `PartitionRangeDef`, `.parseRlsExpr_`, `SubscriptionDef`, `.saveSubscriptions_`, `PublicationDef`, `.applyAndCommit`, `.createMaterializedView`, `.createPublication`, `.truncateTable`, `.createColumnTable`, `.setTableCompression`, `ForeignTableDef`, `.getAllTableNamesInternal`, `.rollbackTransaction`, `.getRlsPoliciesJsonForUser`, `.prepareTx`, `.printInheritanceTree_`, `unique_ptr`, `vector`?**
  _High betweenness centrality (0.493) - this node is a cross-community bridge._
- **Why does `ParsedCommand` connect `ParsedCommand` to `milansql_tests.cpp`, `Column`, `SelectItem`, `Parser`, `WhereCondition`, `testGroup109`, `CursorData`, `PartitionRangeDef`, `.parseRlsExpr_`?**
  _High betweenness centrality (0.289) - this node is a cross-community bridge._
- **Why does `SelectItem` connect `SelectItem` to `ParsedCommand`, `string`, `vector`, `ExplainRequest`, `Row`, `engine.hpp`, `.parseRlsExpr_`?**
  _High betweenness centrality (0.068) - this node is a cross-community bridge._
- **Are the 13 inferred relationships involving `Engine` (e.g. with `testGroup108()` and `testGroup109()`) actually correct?**
  _`Engine` has 13 INFERRED edges - model-reasoned connections that need verification._
- **Are the 2 inferred relationships involving `ParsedCommand` (e.g. with `.splitTrim()` and `.splitValues()`) actually correct?**
  _`ParsedCommand` has 2 INFERRED edges - model-reasoned connections that need verification._
- **Are the 33 inferred relationships involving `Table` (e.g. with `.applyOp()` and `.applyRls_()`) actually correct?**
  _`Table` has 33 INFERRED edges - model-reasoned connections that need verification._
- **What connects `vars`, `cursors`, `notFoundVar` to the rest of the system?**
  _503 weakly-connected nodes found - possible documentation gaps or missing edges._