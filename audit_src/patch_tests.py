import sys

with open('/opt/milansql/src/tests/milansql_tests.cpp', 'r') as f:
    content = f.read()

test_code = '''
// testGroup104 -- Phase 176: Enhanced Query Audit Log
static void testGroup104() {
    std::cout << "\\n-- testGroup104: Enhanced Query Audit Log --\\n";

    milansql::Engine engine;
    milansql::Parser parser;

    auto execSQL = [&](const std::string& sql) -> milansql::QueryResult {
        milansql::ParsedCommand cmd = parser.parse(sql);
        return milansql::dispatch(cmd, engine);
    };

    // Setup: enable audit logging at ALL level
    execSQL("SET AUDIT_LOG = ON");
    execSQL("SET AUDIT_LEVEL = ALL");

    // Create a test table and do some operations
    execSQL("CREATE TABLE audit_test (id INT, name TEXT)");
    execSQL("INSERT INTO audit_test VALUES (1, 'Alice')");
    execSQL("INSERT INTO audit_test VALUES (2, 'Bob')");
    execSQL("SELECT * FROM audit_test");
    execSQL("DROP TABLE audit_test");

    // Test 1: SELECT * FROM _audit_log returns entries
    {
        auto qr = execSQL("SELECT * FROM _audit_log");
        check(qr.error.empty(), "Audit-01: SELECT _audit_log no error");
        check(qr.rows.size() >= 4, "Audit-02: _audit_log has entries (CREATE+2xINSERT+SELECT+DROP)");
        check(qr.columns.size() == 7, "Audit-03: _audit_log has 7 columns");
    }

    // Test 2: Filter by op
    {
        auto qr = execSQL("SELECT * FROM _audit_log WHERE op = 'INSERT'");
        check(qr.error.empty(), "Audit-04: filter by op no error");
        check(qr.rows.size() == 2, "Audit-05: 2 INSERT entries");
    }

    // Test 3: Filter by op (SELECT)
    {
        auto qr = execSQL("SELECT * FROM _audit_log WHERE op = 'SELECT'");
        check(qr.error.empty(), "Audit-06: filter SELECT no error");
        check(qr.rows.size() >= 1, "Audit-07: at least 1 SELECT entry");
    }

    // Test 4: SHOW AUDIT LOG (existing command)
    {
        auto cmd = parser.parse("SHOW AUDIT LOG");
        auto qr = milansql::dispatch(cmd, engine);
        check(qr.error.empty(), "Audit-08: SHOW AUDIT LOG no error");
        check(qr.rows.size() >= 4, "Audit-09: SHOW AUDIT LOG returns entries");
    }

    // Test 5: SHOW AUDIT LOG with LIMIT
    {
        auto cmd = parser.parse("SHOW AUDIT LOG LIMIT 2");
        auto qr = milansql::dispatch(cmd, engine);
        check(qr.error.empty(), "Audit-10: SHOW AUDIT LOG LIMIT no error");
        check(qr.rows.size() == 2, "Audit-11: LIMIT 2 returns 2 entries");
    }

    // Test 6: TRUNCATE _audit_log
    {
        auto qr = execSQL("TRUNCATE TABLE _audit_log");
        check(qr.error.empty(), "Audit-12: TRUNCATE _audit_log no error");
        auto qr2 = execSQL("SELECT * FROM _audit_log");
        check(qr2.rows.size() == 0, "Audit-13: _audit_log empty after TRUNCATE");
    }

    // Test 7: Audit levels - DDL only
    {
        execSQL("SET AUDIT_LEVEL = DDL");
        execSQL("CREATE TABLE audit_lvl (x INT)");
        execSQL("INSERT INTO audit_lvl VALUES (1)");
        execSQL("SELECT * FROM audit_lvl");

        auto qr = execSQL("SELECT * FROM _audit_log");
        bool hasDDL = false, hasDML = false;
        for (const auto& row : qr.rows) {
            if (row.values.size() >= 4) {
                if (row.values[3] == "CREATE_TABLE") hasDDL = true;
                if (row.values[3] == "INSERT" || row.values[3] == "SELECT") hasDML = true;
            }
        }
        check(hasDDL, "Audit-14: DDL level logs CREATE_TABLE");
        check(!hasDML, "Audit-15: DDL level does NOT log INSERT/SELECT");

        execSQL("DROP TABLE audit_lvl");
        execSQL("TRUNCATE TABLE _audit_log");
    }

    // Test 8: Audit levels - DML (DDL + DML but not SELECT)
    {
        execSQL("SET AUDIT_LEVEL = DML");
        execSQL("CREATE TABLE audit_dml (x INT)");
        execSQL("INSERT INTO audit_dml VALUES (1)");
        execSQL("SELECT * FROM audit_dml");

        auto qr = execSQL("SELECT * FROM _audit_log");
        bool hasDDL = false, hasDML = false, hasSELECT = false;
        for (const auto& row : qr.rows) {
            if (row.values.size() >= 4) {
                if (row.values[3] == "CREATE_TABLE") hasDDL = true;
                if (row.values[3] == "INSERT") hasDML = true;
                if (row.values[3] == "SELECT") hasSELECT = true;
            }
        }
        check(hasDDL, "Audit-16: DML level logs CREATE_TABLE");
        check(hasDML, "Audit-17: DML level logs INSERT");
        check(!hasSELECT, "Audit-18: DML level does NOT log SELECT");

        execSQL("DROP TABLE audit_dml");
        execSQL("TRUNCATE TABLE _audit_log");
    }

    // Test 9: Audit level ALL logs SELECT
    {
        execSQL("SET AUDIT_LEVEL = ALL");
        execSQL("CREATE TABLE audit_all (x INT)");
        execSQL("SELECT * FROM audit_all");

        auto qr = execSQL("SELECT * FROM _audit_log");
        bool hasSELECT = false;
        for (const auto& row : qr.rows) {
            if (row.values.size() >= 4 && row.values[3] == "SELECT") hasSELECT = true;
        }
        check(hasSELECT, "Audit-19: ALL level logs SELECT");

        execSQL("DROP TABLE audit_all");
        execSQL("TRUNCATE TABLE _audit_log");
    }

    // Test 10: Audit level OFF logs nothing
    {
        execSQL("SET AUDIT_LEVEL = OFF");
        execSQL("CREATE TABLE audit_off (x INT)");
        execSQL("INSERT INTO audit_off VALUES (1)");

        auto qr = execSQL("SELECT * FROM _audit_log");
        check(qr.rows.size() == 0, "Audit-20: OFF level logs nothing");

        // Re-enable for cleanup
        execSQL("SET AUDIT_LEVEL = ALL");
        execSQL("DROP TABLE audit_off");
        execSQL("TRUNCATE TABLE _audit_log");
    }

    // Test 11: DELETE FROM _audit_log WHERE user = 'root'
    {
        execSQL("CREATE TABLE audit_del (x INT)");
        execSQL("INSERT INTO audit_del VALUES (1)");
        auto qr1 = execSQL("SELECT * FROM _audit_log");
        size_t before = qr1.rows.size();
        check(before >= 2, "Audit-21: entries before DELETE");

        execSQL("DELETE FROM _audit_log WHERE user = 'root'");
        auto qr2 = execSQL("SELECT * FROM _audit_log");
        check(qr2.rows.size() == 0, "Audit-22: all root entries deleted");

        execSQL("DROP TABLE audit_del");
        execSQL("TRUNCATE TABLE _audit_log");
    }

    // Test 12: SHOW AUDIT STATUS
    {
        auto qr = execSQL("SHOW AUDIT STATUS");
        check(qr.error.empty(), "Audit-23: SHOW AUDIT STATUS no error");
        check(qr.columns.size() == 2, "Audit-24: SHOW AUDIT STATUS 2 columns");
        bool hasEnabled = false, hasLevel = false;
        for (const auto& row : qr.rows) {
            if (row.values.size() >= 2) {
                if (row.values[0] == "enabled") hasEnabled = true;
                if (row.values[0] == "level") hasLevel = true;
            }
        }
        check(hasEnabled, "Audit-25: SHOW AUDIT STATUS has enabled");
        check(hasLevel, "Audit-26: SHOW AUDIT STATUS has level");
    }

    // Test 13: Filter by table
    {
        execSQL("CREATE TABLE audit_tbl (x INT)");
        execSQL("INSERT INTO audit_tbl VALUES (1)");
        auto qr = execSQL("SELECT * FROM _audit_log WHERE table = 'audit_tbl'");
        check(qr.error.empty(), "Audit-27: filter by table no error");
        check(qr.rows.size() >= 2, "Audit-28: entries for audit_tbl");
        execSQL("DROP TABLE audit_tbl");
        execSQL("TRUNCATE TABLE _audit_log");
    }

    // Test 14: _audit_log with LIMIT
    {
        execSQL("CREATE TABLE audit_lim (x INT)");
        for (int i = 0; i < 5; ++i)
            execSQL("INSERT INTO audit_lim VALUES (" + std::to_string(i) + ")");
        auto qr = execSQL("SELECT * FROM _audit_log LIMIT 3");
        check(qr.error.empty(), "Audit-29: _audit_log LIMIT no error");
        check(qr.rows.size() == 3, "Audit-30: _audit_log LIMIT 3 returns 3");
        execSQL("DROP TABLE audit_lim");
        execSQL("TRUNCATE TABLE _audit_log");
    }

    // Cleanup: disable audit
    execSQL("SET AUDIT_LOG = OFF");

    std::cout << "  testGroup104 passed.\\n";
}

'''

content = content.replace('// MAIN\n', test_code + '// MAIN\n', 1)

# Add testGroup104 call in main() after testGroup103
old_call = '''    try { testGroup103(); } catch (const std::exception& e) {
        std::cout << "[ERROR] Group 103 exception: " << e.what() << "\\n"; ++failed;
    }

    std::cout'''

new_call = '''    try { testGroup103(); } catch (const std::exception& e) {
        std::cout << "[ERROR] Group 103 exception: " << e.what() << "\\n"; ++failed;
    }
    try { testGroup104(); } catch (const std::exception& e) {
        std::cout << "[ERROR] Group 104 exception: " << e.what() << "\\n"; ++failed;
    }

    std::cout'''

content = content.replace(old_call, new_call)

with open('/opt/milansql/src/tests/milansql_tests.cpp', 'w') as f:
    f.write(content)

print('testGroup104 added.')
