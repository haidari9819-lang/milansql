// ============================================================
// milansql_tests.cpp — Automatisierte Testsuite für MilanSQL
// Phase 39.5: Production-Level Stabilization
// ============================================================

#include <iostream>
#include <string>
#include <vector>
#include <cassert>
#include <stdexcept>
#include <cstdio>

#include "engine/engine.hpp"
#include "engine/btree.hpp"
#include "parser/parser.hpp"

// ── Statistik ──────────────────────────────────────────────────
static int passed = 0;
static int failed = 0;

static void check(bool condition, const std::string& testName) {
    if (condition) {
        std::cout << "[PASS] " << testName << "\n";
        ++passed;
    } else {
        std::cout << "[FAIL] " << testName << "\n";
        ++failed;
    }
}

// ── Helper: SELECT-Abfrage als Table ausführen ─────────────────
// Vereinfachte Version aus main.cpp: nur einfaches SELECT und SET-OPs
static milansql::Table executeSelect(
        milansql::Engine& engine,
        milansql::Parser& parser,
        const std::string& sql)
{
    milansql::ParsedCommand cmd = parser.parse(sql);

    // Subqueries auflösen
    for (const auto& sq : cmd.subqueries) {
        if (sq.condIdx < cmd.whereConds.size()) {
            cmd.whereConds[sq.condIdx].inList =
                engine.subqueryValues(sq.subTable, sq.subCol,
                                      sq.subWhere, sq.subWhereLogic);
        }
    }

    // SET-Operationen
    if (cmd.isSetOp) {
        milansql::Table leftResult;
        if (!cmd.whereConds.empty()) {
            auto qr = engine.selectWhere(cmd.tableName, cmd.whereConds, cmd.whereLogic);
            leftResult = std::move(qr.table);
        } else {
            leftResult = engine.selectAll(cmd.tableName).clone();
        }
        if (!cmd.selectColumns.empty())
            leftResult = leftResult.project(cmd.selectColumns);

        milansql::ParsedCommand rc = parser.parse(cmd.rightSql);
        for (const auto& sq : rc.subqueries) {
            if (sq.condIdx < rc.whereConds.size())
                rc.whereConds[sq.condIdx].inList =
                    engine.subqueryValues(sq.subTable, sq.subCol,
                                          sq.subWhere, sq.subWhereLogic);
        }
        milansql::Table rightResult;
        if (!rc.whereConds.empty()) {
            auto qr = engine.selectWhere(rc.tableName, rc.whereConds, rc.whereLogic);
            rightResult = std::move(qr.table);
        } else {
            rightResult = engine.selectAll(rc.tableName).clone();
        }
        if (!rc.selectColumns.empty())
            rightResult = rightResult.project(rc.selectColumns);

        milansql::Table result = engine.executeSetOp(leftResult, cmd.setOp, rightResult);
        if (!cmd.orderByCols.empty()) result.sortByMulti(cmd.orderByCols);
        return result;
    }

    // JOIN
    if (cmd.isJoin) {
        auto result = engine.executeJoins(
            cmd.tableName, cmd.joinClauses,
            cmd.whereConds, cmd.whereLogic);
        if (!cmd.orderByCols.empty()) result.sortByMulti(cmd.orderByCols);
        if (!cmd.selectColumns.empty())
            result = result.project(cmd.selectColumns);
        return result;
    }

    // GROUP BY
    if (cmd.isGroupBy) {
        auto result = engine.groupBy(
            cmd.tableName,
            cmd.whereConds, cmd.whereLogic,
            cmd.groupByCols,
            cmd.selectItems,
            cmd.havingConds, cmd.havingLogic);
        if (!cmd.orderByCols.empty()) result.sortByMulti(cmd.orderByCols);
        return result;
    }

    // Aggregat (COUNT)
    if (cmd.isCount) {
        std::size_t n = engine.countWhere(
            cmd.tableName, cmd.whereConds, cmd.whereLogic);
        // Ergebnis als 1-Zeilen-Tabelle mit Spalte "COUNT(*)"
        std::vector<milansql::Column> cols;
        cols.emplace_back("COUNT(*)", "INT");
        milansql::Table result("", cols);
        result.insert(milansql::Row({std::to_string(n)}));
        return result;
    }

    // Aggregat (MIN/MAX/AVG/SUM)
    if (cmd.isAggregate) {
        std::string val = engine.computeAggregate(
            cmd.tableName, cmd.aggFunc, cmd.aggCol,
            cmd.whereConds, cmd.whereLogic);
        std::string colName = cmd.aggFunc + "(" + cmd.aggCol + ")";
        std::vector<milansql::Column> cols;
        cols.emplace_back(colName, "TEXT");
        milansql::Table result("", cols);
        result.insert(milansql::Row({val}));
        return result;
    }

    // Einfaches SELECT
    milansql::Table result;
    if (!cmd.whereConds.empty()) {
        auto qr = engine.selectWhere(cmd.tableName, cmd.whereConds, cmd.whereLogic);
        result = std::move(qr.table);
    } else {
        result = engine.selectAll(cmd.tableName).clone();
    }

    if (cmd.hasCaseItems && !cmd.selectItems.empty()) {
        result = engine.projectWithItems(result, cmd.selectItems);
    } else {
        if (!cmd.selectColumns.empty())
            result = result.project(cmd.selectColumns);
    }
    if (!cmd.orderByCols.empty()) result.sortByMulti(cmd.orderByCols);
    if (cmd.isDistinct) result.makeDistinct();
    return result;
}

// ── Helper: SQL ausführen (DDL/DML) ────────────────────────────
static void execSQL(milansql::Engine& engine, milansql::Parser& parser,
                    const std::string& sql) {
    milansql::ParsedCommand cmd = parser.parse(sql);
    switch (cmd.type) {
        case milansql::CommandType::CREATE_TABLE:
            engine.createTable(cmd.tableName, cmd.columns, cmd.foreignKeys);
            break;
        case milansql::CommandType::INSERT: {
            const auto& rows = cmd.multiValues.empty()
                ? std::vector<std::vector<std::string>>{cmd.values}
                : cmd.multiValues;
            for (const auto& vals : rows)
                engine.insertRow(cmd.tableName, vals);
            break;
        }
        case milansql::CommandType::DELETE:
            if (!cmd.whereColumn.empty())
                engine.deleteWhere(cmd.tableName, cmd.whereColumn, cmd.whereValue);
            else
                engine.deleteAll(cmd.tableName);
            break;
        case milansql::CommandType::UPDATE:
            if (!cmd.whereColumn.empty())
                engine.updateWhere(cmd.tableName, cmd.updateCols, cmd.updateVals,
                                   cmd.whereColumn, cmd.whereValue);
            else
                engine.updateAll(cmd.tableName, cmd.updateCols, cmd.updateVals);
            break;
        case milansql::CommandType::BEGIN:
            engine.beginTransaction("/tmp/test_milansql.wal");
            break;
        case milansql::CommandType::COMMIT:
            engine.applyAndCommit();
            break;
        case milansql::CommandType::ROLLBACK:
            engine.rollbackTransaction();
            break;
        case milansql::CommandType::CREATE_INDEX:
            engine.createIndex(cmd.tableName, cmd.indexColumns, cmd.indexName);
            break;
        case milansql::CommandType::DROP_TABLE:
            engine.dropTable(cmd.tableName);
            break;
        default:
            break;
    }
}

// ── Hilfsfunktion: Zellwert einer Zeile nach Spaltenname ────────
static std::string cellVal(const milansql::Table& tbl, size_t row, const std::string& colName) {
    const auto& cols = tbl.columns();
    for (size_t i = 0; i < cols.size(); ++i) {
        if (cols[i].name == colName && row < tbl.rows().size())
            return tbl.rows()[row].values[i];
    }
    return "__NOT_FOUND__";
}

// ══════════════════════════════════════════════════════════════
// TEST GROUPS
// ══════════════════════════════════════════════════════════════

// ── Group 1: Basic DDL/DML ─────────────────────────────────────
static void testGroup1() {
    std::cout << "\n--- Group 1: Basic DDL/DML ---\n";
    milansql::Engine engine;
    milansql::Parser parser;

    execSQL(engine, parser,
        "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name TEXT NOT NULL, age INT)");

    execSQL(engine, parser, "INSERT INTO users VALUES (NULL, Alice, 30)");
    execSQL(engine, parser, "INSERT INTO users VALUES (NULL, Bob, 25)");

    {
        auto tbl = executeSelect(engine, parser, "SELECT * FROM users");
        check(tbl.rowCount() == 2, "SELECT * → 2 rows");
    }
    {
        auto tbl = executeSelect(engine, parser, "SELECT * FROM users WHERE age > 27");
        check(tbl.rowCount() == 1, "WHERE age > 27 → 1 row");
        check(cellVal(tbl, 0, "name") == "Alice", "WHERE age > 27 → Alice");
    }
    {
        auto tbl = executeSelect(engine, parser, "SELECT * FROM users ORDER BY age ASC");
        check(tbl.rowCount() == 2, "ORDER BY age → 2 rows");
        check(cellVal(tbl, 0, "name") == "Bob", "ORDER BY age ASC → Bob first");
    }
    {
        // LIMIT is enforced at display-time in main.cpp via printTable.
        // The engine returns all matching rows; tests verify the full result
        // and rely on the cmd.limit field for the count check.
        milansql::ParsedCommand limitCmd = parser.parse("SELECT * FROM users LIMIT 1");
        milansql::Table tbl = executeSelect(engine, parser, "SELECT * FROM users");
        // Apply limit manually as printTable does
        size_t printRows = (limitCmd.limit >= 0 &&
                            static_cast<size_t>(limitCmd.limit) < tbl.rowCount())
                           ? static_cast<size_t>(limitCmd.limit) : tbl.rowCount();
        check(printRows == 1, "LIMIT 1 → printRows = 1");
    }
}

// ── Group 2: Aggregation ────────────────────────────────────────
static void testGroup2() {
    std::cout << "\n--- Group 2: Aggregation ---\n";
    milansql::Engine engine;
    milansql::Parser parser;

    execSQL(engine, parser,
        "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name TEXT, age INT)");
    execSQL(engine, parser, "INSERT INTO users VALUES (NULL, Alice, 30)");
    execSQL(engine, parser, "INSERT INTO users VALUES (NULL, Bob, 25)");

    {
        auto tbl = executeSelect(engine, parser, "SELECT COUNT(*) FROM users");
        check(tbl.rowCount() == 1, "COUNT(*) → 1 result row");
        check(tbl.rows()[0].values[0] == "2", "COUNT(*) = 2");
    }
    {
        auto tbl = executeSelect(engine, parser, "SELECT AVG(age) FROM users");
        check(tbl.rowCount() == 1, "AVG(age) → 1 result row");
        check(tbl.rows()[0].values[0] == "27.5", "AVG(age) = 27.5");
    }
    {
        auto tbl = executeSelect(engine, parser, "SELECT MAX(age) FROM users");
        check(tbl.rows()[0].values[0] == "30", "MAX(age) = 30");
    }
    {
        auto tbl = executeSelect(engine, parser, "SELECT MIN(age) FROM users");
        check(tbl.rows()[0].values[0] == "25", "MIN(age) = 25");
    }
    {
        // GROUP BY with HAVING on aggregate
        // Alice (30) → COUNT(*)=1, Bob (25) → COUNT(*)=1
        // HAVING COUNT(*) >= 1 → both groups pass → 2 rows
        auto tbl = executeSelect(engine, parser,
            "SELECT age, COUNT(*) FROM users GROUP BY age HAVING COUNT(*) >= 1");
        check(tbl.rowCount() == 2, "GROUP BY age HAVING COUNT(*)>=1 → 2 groups");
    }
}

// ── Group 3: JOINs ─────────────────────────────────────────────
static void testGroup3() {
    std::cout << "\n--- Group 3: JOINs ---\n";
    milansql::Engine engine;
    milansql::Parser parser;

    execSQL(engine, parser,
        "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name TEXT)");
    execSQL(engine, parser,
        "CREATE TABLE orders (id INT PRIMARY KEY AUTO_INCREMENT, user_id INT, amount INT)");

    execSQL(engine, parser, "INSERT INTO users VALUES (NULL, Alice)");
    execSQL(engine, parser, "INSERT INTO users VALUES (NULL, Bob)");
    execSQL(engine, parser, "INSERT INTO orders VALUES (NULL, 1, 100)");
    execSQL(engine, parser, "INSERT INTO orders VALUES (NULL, 1, 200)");
    execSQL(engine, parser, "INSERT INTO orders VALUES (NULL, 2, 50)");

    {
        auto tbl = executeSelect(engine, parser,
            "SELECT * FROM users INNER JOIN orders ON users.id = orders.user_id");
        check(tbl.rowCount() == 3, "INNER JOIN → 3 rows (Alice×2, Bob×1)");
    }
    {
        auto tbl = executeSelect(engine, parser,
            "SELECT * FROM users LEFT JOIN orders ON users.id = orders.user_id");
        // Both users have orders, so still 3
        check(tbl.rowCount() == 3, "LEFT JOIN → 3 rows (all users covered)");
    }

    // Add a user with no orders to test LEFT JOIN NULL row
    execSQL(engine, parser, "INSERT INTO users VALUES (NULL, Charlie)");
    {
        auto tbl = executeSelect(engine, parser,
            "SELECT * FROM users LEFT JOIN orders ON users.id = orders.user_id");
        check(tbl.rowCount() == 4, "LEFT JOIN with unmatched → 4 rows (Charlie+NULL)");
    }
}

// ── Group 4: Transactions ───────────────────────────────────────
static void testGroup4() {
    std::cout << "\n--- Group 4: Transactions ---\n";
    milansql::Engine engine;
    milansql::Parser parser;

    execSQL(engine, parser,
        "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name TEXT, age INT)");
    execSQL(engine, parser, "INSERT INTO users VALUES (NULL, Alice, 30)");
    execSQL(engine, parser, "INSERT INTO users VALUES (NULL, Bob, 25)");

    // ROLLBACK test
    execSQL(engine, parser, "BEGIN");
    execSQL(engine, parser, "INSERT INTO users VALUES (NULL, Charlie, 35)");
    execSQL(engine, parser, "ROLLBACK");

    {
        auto tbl = executeSelect(engine, parser, "SELECT * FROM users");
        check(tbl.rowCount() == 2, "After ROLLBACK → still 2 rows");
    }

    // COMMIT test
    execSQL(engine, parser, "BEGIN");
    execSQL(engine, parser, "INSERT INTO users VALUES (NULL, Dave, 40)");
    execSQL(engine, parser, "COMMIT");

    {
        auto tbl = executeSelect(engine, parser, "SELECT * FROM users");
        check(tbl.rowCount() == 3, "After COMMIT → 3 rows (Dave committed)");
        check(cellVal(tbl, 2, "name") == "Dave", "Dave is in DB after COMMIT");
    }
}

// ── Group 5: Constraints ────────────────────────────────────────
static void testGroup5() {
    std::cout << "\n--- Group 5: Constraints ---\n";
    milansql::Engine engine;
    milansql::Parser parser;

    execSQL(engine, parser,
        "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name TEXT NOT NULL, age INT)");

    // AUTO_INCREMENT: NULL id → auto-assigned
    execSQL(engine, parser, "INSERT INTO users VALUES (NULL, Alice, 30)");
    execSQL(engine, parser, "INSERT INTO users VALUES (NULL, Bob, 25)");
    {
        auto tbl = executeSelect(engine, parser, "SELECT * FROM users");
        check(cellVal(tbl, 0, "id") == "1", "AUTO_INCREMENT: first row id=1");
        check(cellVal(tbl, 1, "id") == "2", "AUTO_INCREMENT: second row id=2");
    }

    // NOT NULL violation
    {
        bool threw = false;
        try {
            execSQL(engine, parser, "INSERT INTO users VALUES (NULL, NULL, 99)");
        } catch (const std::exception&) {
            threw = true;
        }
        check(threw, "NOT NULL: inserting NULL in NOT NULL column throws");
    }

    // UNIQUE constraint
    {
        milansql::Engine eng2;
        milansql::Parser par2;
        execSQL(eng2, par2, "CREATE TABLE t (id INT UNIQUE, name TEXT)");
        execSQL(eng2, par2, "INSERT INTO t VALUES (1, Alice)");
        bool threw = false;
        try {
            execSQL(eng2, par2, "INSERT INTO t VALUES (1, Bob)");
        } catch (const std::exception&) {
            threw = true;
        }
        check(threw, "UNIQUE: duplicate value throws");
    }

    // CHECK constraint
    {
        milansql::Engine eng3;
        milansql::Parser par3;
        execSQL(eng3, par3,
            "CREATE TABLE products (id INT, price INT CHECK (price >= 0))");
        bool threw = false;
        try {
            execSQL(eng3, par3, "INSERT INTO products VALUES (1, -5)");
        } catch (const std::exception&) {
            threw = true;
        }
        check(threw, "CHECK: negative price throws");

        // Valid insert should succeed
        bool ok = true;
        try {
            execSQL(eng3, par3, "INSERT INTO products VALUES (2, 10)");
        } catch (const std::exception&) {
            ok = false;
        }
        check(ok, "CHECK: valid price (>=0) inserts fine");
    }
}

// ── Group 6: Foreign Keys + CASCADE ────────────────────────────
static void testGroup6() {
    std::cout << "\n--- Group 6: Foreign Keys + CASCADE ---\n";
    milansql::Engine engine;
    milansql::Parser parser;

    execSQL(engine, parser,
        "CREATE TABLE parent (id INT PRIMARY KEY AUTO_INCREMENT, name TEXT)");
    execSQL(engine, parser,
        "CREATE TABLE child (id INT PRIMARY KEY AUTO_INCREMENT, parent_id INT, "
        "FOREIGN KEY (parent_id) REFERENCES parent(id) ON DELETE CASCADE)");

    execSQL(engine, parser, "INSERT INTO parent VALUES (NULL, Alpha)");
    execSQL(engine, parser, "INSERT INTO parent VALUES (NULL, Beta)");
    execSQL(engine, parser, "INSERT INTO child VALUES (NULL, 1)");
    execSQL(engine, parser, "INSERT INTO child VALUES (NULL, 1)");
    execSQL(engine, parser, "INSERT INTO child VALUES (NULL, 2)");

    {
        auto tbl = executeSelect(engine, parser, "SELECT * FROM child");
        check(tbl.rowCount() == 3, "Before cascade delete: 3 child rows");
    }

    // Delete parent id=1 → should cascade-delete children with parent_id=1
    execSQL(engine, parser, "DELETE FROM parent WHERE id = 1");

    {
        auto tbl = executeSelect(engine, parser, "SELECT * FROM child");
        check(tbl.rowCount() == 1, "After CASCADE DELETE: 1 child row remains");
    }
}

// ── Group 7: Set Operations ─────────────────────────────────────
static void testGroup7() {
    std::cout << "\n--- Group 7: Set Operations ---\n";
    milansql::Engine engine;
    milansql::Parser parser;

    execSQL(engine, parser, "CREATE TABLE t1 (name TEXT)");
    execSQL(engine, parser, "CREATE TABLE t2 (name TEXT)");
    execSQL(engine, parser, "INSERT INTO t1 VALUES (Alice)");
    execSQL(engine, parser, "INSERT INTO t1 VALUES (Bob)");
    execSQL(engine, parser, "INSERT INTO t2 VALUES (Bob)");
    execSQL(engine, parser, "INSERT INTO t2 VALUES (Charlie)");

    {
        auto tbl = executeSelect(engine, parser,
            "SELECT name FROM t1 UNION SELECT name FROM t2");
        check(tbl.rowCount() == 3, "UNION: 3 unique names (Alice, Bob, Charlie)");
    }
    {
        auto tbl = executeSelect(engine, parser,
            "SELECT name FROM t1 UNION ALL SELECT name FROM t2");
        check(tbl.rowCount() == 4, "UNION ALL: 4 rows with duplicates");
    }
    {
        auto tbl = executeSelect(engine, parser,
            "SELECT name FROM t1 INTERSECT SELECT name FROM t2");
        check(tbl.rowCount() == 1, "INTERSECT: only Bob");
        check(tbl.rows()[0].values[0] == "Bob", "INTERSECT value = Bob");
    }
    {
        auto tbl = executeSelect(engine, parser,
            "SELECT name FROM t1 EXCEPT SELECT name FROM t2");
        check(tbl.rowCount() == 1, "EXCEPT: only Alice");
        check(tbl.rows()[0].values[0] == "Alice", "EXCEPT value = Alice");
    }
}

// ── Group 8: CASE WHEN ──────────────────────────────────────────
static void testGroup8() {
    std::cout << "\n--- Group 8: CASE WHEN ---\n";
    milansql::Engine engine;
    milansql::Parser parser;

    execSQL(engine, parser,
        "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name TEXT, age INT)");
    execSQL(engine, parser, "INSERT INTO users VALUES (NULL, Alice, 30)");
    execSQL(engine, parser, "INSERT INTO users VALUES (NULL, Bob, 25)");

    {
        auto tbl = executeSelect(engine, parser,
            "SELECT CASE WHEN age > 28 THEN senior ELSE junior END AS cat FROM users "
            "ORDER BY name ASC");
        check(tbl.rowCount() == 2, "CASE WHEN → 2 rows");
        // ORDER BY name ASC: Alice (30) → senior, Bob (25) → junior
        check(tbl.rows()[0].values[0] == "senior", "Alice → senior");
        check(tbl.rows()[1].values[0] == "junior", "Bob → junior");
    }
}

// ── Group 9: WITH / CTE ─────────────────────────────────────────
static void testGroup9() {
    std::cout << "\n--- Group 9: WITH / CTE ---\n";
    milansql::Engine engine;
    milansql::Parser parser;

    execSQL(engine, parser,
        "CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name TEXT, age INT)");
    execSQL(engine, parser, "INSERT INTO users VALUES (NULL, Alice, 30)");
    execSQL(engine, parser, "INSERT INTO users VALUES (NULL, Bob, 25)");
    execSQL(engine, parser, "INSERT INTO users VALUES (NULL, Carol, 22)");

    {
        // Simple CTE: seniors = users WHERE age > 27
        milansql::ParsedCommand cmd = parser.parse(
            "WITH seniors AS (SELECT * FROM users WHERE age > 27) "
            "SELECT * FROM seniors");

        // Execute CTE manually
        for (auto& [cteName, cteInnerSql] : cmd.cteList) {
            milansql::ParsedCommand cteParsed = parser.parse(cteInnerSql);
            milansql::Table cteResult = executeSelect(engine, parser, cteInnerSql);
            engine.registerTempTable(cteName, std::move(cteResult));
        }
        // Run main query against temp table
        milansql::Table mainResult = executeSelect(engine, parser,
            "SELECT * FROM seniors");
        engine.cleanupTempTables();

        check(mainResult.rowCount() == 1, "CTE seniors (age>27) → 1 row");
        check(cellVal(mainResult, 0, "name") == "Alice", "CTE seniors → Alice");
    }
}

// ── Group 10: B-Tree Index ──────────────────────────────────────
static void testGroup10() {
    std::cout << "\n--- Group 10: B-Tree Index ---\n";
    milansql::Engine engine;
    milansql::Parser parser;

    execSQL(engine, parser,
        "CREATE TABLE items (id INT PRIMARY KEY AUTO_INCREMENT, val INT, name TEXT)");

    // Insert 100 rows
    for (int i = 1; i <= 100; ++i) {
        execSQL(engine, parser,
            "INSERT INTO items VALUES (NULL, " + std::to_string(i) + ", item" + std::to_string(i) + ")");
    }

    // Without index
    {
        auto tbl = executeSelect(engine, parser, "SELECT * FROM items WHERE val = 42");
        check(tbl.rowCount() == 1, "SELECT val=42 without index → 1 row");
    }

    // Create index on val
    execSQL(engine, parser, "CREATE INDEX idx_val ON items (val)");

    // With index
    {
        auto tbl = executeSelect(engine, parser, "SELECT * FROM items WHERE val = 42");
        check(tbl.rowCount() == 1, "SELECT val=42 WITH index → 1 row");
        check(cellVal(tbl, 0, "name") == "item42", "Index search returns correct row");
    }
}

// ══════════════════════════════════════════════════════════════
// MAIN
// ══════════════════════════════════════════════════════════════

int main() {
    std::cout << "========================================\n";
    std::cout << "  MilanSQL Test Suite (Phase 39.5)\n";
    std::cout << "========================================\n";

    try { testGroup1(); } catch (const std::exception& e) {
        std::cout << "[ERROR] Group 1 exception: " << e.what() << "\n"; ++failed;
    }
    try { testGroup2(); } catch (const std::exception& e) {
        std::cout << "[ERROR] Group 2 exception: " << e.what() << "\n"; ++failed;
    }
    try { testGroup3(); } catch (const std::exception& e) {
        std::cout << "[ERROR] Group 3 exception: " << e.what() << "\n"; ++failed;
    }
    try { testGroup4(); } catch (const std::exception& e) {
        std::cout << "[ERROR] Group 4 exception: " << e.what() << "\n"; ++failed;
    }
    try { testGroup5(); } catch (const std::exception& e) {
        std::cout << "[ERROR] Group 5 exception: " << e.what() << "\n"; ++failed;
    }
    try { testGroup6(); } catch (const std::exception& e) {
        std::cout << "[ERROR] Group 6 exception: " << e.what() << "\n"; ++failed;
    }
    try { testGroup7(); } catch (const std::exception& e) {
        std::cout << "[ERROR] Group 7 exception: " << e.what() << "\n"; ++failed;
    }
    try { testGroup8(); } catch (const std::exception& e) {
        std::cout << "[ERROR] Group 8 exception: " << e.what() << "\n"; ++failed;
    }
    try { testGroup9(); } catch (const std::exception& e) {
        std::cout << "[ERROR] Group 9 exception: " << e.what() << "\n"; ++failed;
    }
    try { testGroup10(); } catch (const std::exception& e) {
        std::cout << "[ERROR] Group 10 exception: " << e.what() << "\n"; ++failed;
    }

    std::cout << "\n========================================\n";
    std::cout << "  Results: " << passed << " passed, " << failed << " failed\n";
    std::cout << "========================================\n";

    // Cleanup temp WAL if any
    std::remove("/tmp/test_milansql.wal");

    return failed > 0 ? 1 : 0;
}
