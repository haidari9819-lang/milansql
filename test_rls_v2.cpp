// test_rls_v2.cpp — Phase 170: Advanced RLS tests
// Tests: AND/OR, comparison operators, IN, WITH CHECK, UPDATE/DELETE enforcement
#include <cassert>
#include <iostream>
#include <string>
#include <sstream>
#include "../milansql/src/engine/engine.hpp"

static int passed = 0;
static int failed = 0;

#define TEST(name) { std::cout << "  TEST: " << name << " ... "; }
#define PASS() { std::cout << "OK\n"; ++passed; }
#define FAIL(msg) { std::cout << "FAIL: " << msg << "\n"; ++failed; }

void test_rls_and_or() {
    TEST("RLS with AND conditions");
    milansql::Engine engine;
    engine.createTable("docs", {{"id","INT"}, {"owner","TEXT"}, {"status","TEXT"}, {"dept","TEXT"}});
    engine.setCurrentUserDirect("alice");
    engine.enableRls("docs");

    // Policy: owner = 'alice' AND status = 'active'
    milansql::Engine::RlsPolicy pol;
    pol.name = "owner_active"; pol.table = "docs"; pol.command = "ALL";
    pol.role = "PUBLIC"; pol.usingExpr = "owner = 'alice' AND status = 'active'";
    engine.createRlsPolicy(pol);

    engine.setCurrentUserDirect("root");
    engine.insertRow("docs", {"1", "alice", "active", "eng"});
    engine.insertRow("docs", {"2", "alice", "archived", "eng"});
    engine.insertRow("docs", {"3", "bob",   "active", "eng"});

    engine.setCurrentUserDirect("alice");
    auto result = engine.selectAllFiltered("docs");
    // Only row 1 should match (alice + active)
    if (result.rows().size() == 1 && result.rows()[0].values[0] == "1") {
        PASS();
    } else {
        FAIL("Expected 1 row, got " + std::to_string(result.rows().size()));
    }
}

void test_rls_or() {
    TEST("RLS with OR conditions");
    milansql::Engine engine;
    engine.createTable("items", {{"id","INT"}, {"category","TEXT"}});
    engine.enableRls("items");

    milansql::Engine::RlsPolicy pol;
    pol.name = "cat_filter"; pol.table = "items"; pol.command = "SELECT";
    pol.role = "PUBLIC"; pol.usingExpr = "category = 'books' OR category = 'music'";
    engine.createRlsPolicy(pol);

    engine.setCurrentUserDirect("root");
    engine.insertRow("items", {"1", "books"});
    engine.insertRow("items", {"2", "electronics"});
    engine.insertRow("items", {"3", "music"});
    engine.insertRow("items", {"4", "food"});

    engine.setCurrentUserDirect("user1");
    auto result = engine.selectAllFiltered("items");
    if (result.rows().size() == 2) {
        PASS();
    } else {
        FAIL("Expected 2 rows, got " + std::to_string(result.rows().size()));
    }
}

void test_rls_comparison_operators() {
    TEST("RLS with >, <, >=, <=, != operators");
    milansql::Engine engine;
    engine.createTable("products", {{"id","INT"}, {"price","INT"}, {"name","TEXT"}});
    engine.enableRls("products");

    // Policy: price >= 10 AND price <= 50
    milansql::Engine::RlsPolicy pol;
    pol.name = "price_range"; pol.table = "products"; pol.command = "SELECT";
    pol.role = "PUBLIC"; pol.usingExpr = "price >= 10 AND price <= 50";
    engine.createRlsPolicy(pol);

    engine.setCurrentUserDirect("root");
    engine.insertRow("products", {"1", "5",  "cheap"});
    engine.insertRow("products", {"2", "10", "min"});
    engine.insertRow("products", {"3", "30", "mid"});
    engine.insertRow("products", {"4", "50", "max"});
    engine.insertRow("products", {"5", "100","expensive"});

    engine.setCurrentUserDirect("user1");
    auto result = engine.selectAllFiltered("products");
    // Should see rows 2, 3, 4 (price 10, 30, 50)
    if (result.rows().size() == 3) {
        PASS();
    } else {
        FAIL("Expected 3 rows, got " + std::to_string(result.rows().size()));
    }
}

void test_rls_not_equal() {
    TEST("RLS with != operator");
    milansql::Engine engine;
    engine.createTable("logs", {{"id","INT"}, {"level","TEXT"}});
    engine.enableRls("logs");

    milansql::Engine::RlsPolicy pol;
    pol.name = "no_debug"; pol.table = "logs"; pol.command = "SELECT";
    pol.role = "PUBLIC"; pol.usingExpr = "level != 'debug'";
    engine.createRlsPolicy(pol);

    engine.setCurrentUserDirect("root");
    engine.insertRow("logs", {"1", "info"});
    engine.insertRow("logs", {"2", "debug"});
    engine.insertRow("logs", {"3", "error"});

    engine.setCurrentUserDirect("user1");
    auto result = engine.selectAllFiltered("logs");
    if (result.rows().size() == 2) {
        PASS();
    } else {
        FAIL("Expected 2 rows, got " + std::to_string(result.rows().size()));
    }
}

void test_rls_in_operator() {
    TEST("RLS with IN operator");
    milansql::Engine engine;
    engine.createTable("tasks", {{"id","INT"}, {"status","TEXT"}, {"assignee","TEXT"}});
    engine.enableRls("tasks");

    milansql::Engine::RlsPolicy pol;
    pol.name = "status_filter"; pol.table = "tasks"; pol.command = "SELECT";
    pol.role = "PUBLIC"; pol.usingExpr = "status IN ('open', 'in_progress')";
    engine.createRlsPolicy(pol);

    engine.setCurrentUserDirect("root");
    engine.insertRow("tasks", {"1", "open", "alice"});
    engine.insertRow("tasks", {"2", "closed", "bob"});
    engine.insertRow("tasks", {"3", "in_progress", "carol"});
    engine.insertRow("tasks", {"4", "done", "dave"});

    engine.setCurrentUserDirect("user1");
    auto result = engine.selectAllFiltered("tasks");
    if (result.rows().size() == 2) {
        PASS();
    } else {
        FAIL("Expected 2 rows (open + in_progress), got " + std::to_string(result.rows().size()));
    }
}

void test_rls_current_user_id() {
    TEST("RLS with CURRENT_USER_ID() in complex expr");
    milansql::Engine engine;
    engine.createTable("notes", {{"id","INT"}, {"owner","TEXT"}, {"public","INT"}});
    engine.enableRls("notes");

    // Can see own notes OR public notes
    milansql::Engine::RlsPolicy pol;
    pol.name = "own_or_public"; pol.table = "notes"; pol.command = "SELECT";
    pol.role = "PUBLIC"; pol.usingExpr = "owner = CURRENT_USER_ID() OR public = 1";
    engine.createRlsPolicy(pol);

    engine.setCurrentUserDirect("root");
    engine.insertRow("notes", {"1", "alice", "0"});  // alice's private
    engine.insertRow("notes", {"2", "bob",   "1"});  // bob's public
    engine.insertRow("notes", {"3", "bob",   "0"});  // bob's private
    engine.insertRow("notes", {"4", "alice", "1"});  // alice's public

    engine.setCurrentUserDirect("alice");
    auto result = engine.selectAllFiltered("notes");
    // alice sees: her own (1,4) + public (2) = 3 rows
    if (result.rows().size() == 3) {
        PASS();
    } else {
        FAIL("Expected 3 rows, got " + std::to_string(result.rows().size()));
    }
}

void test_rls_update_enforcement() {
    TEST("RLS blocks UPDATE on rows user cannot see");
    milansql::Engine engine;
    engine.createTable("records", {{"id","INT"}, {"owner","TEXT"}, {"data","TEXT"}});
    engine.enableRls("records");

    milansql::Engine::RlsPolicy pol;
    pol.name = "own_only"; pol.table = "records"; pol.command = "ALL";
    pol.role = "PUBLIC"; pol.usingExpr = "owner = CURRENT_USER_ID()";
    engine.createRlsPolicy(pol);

    engine.setCurrentUserDirect("root");
    engine.insertRow("records", {"1", "alice", "secret"});
    engine.insertRow("records", {"2", "bob",   "data"});

    // Bob tries to update alice's row
    engine.setCurrentUserDirect("bob");
    bool denied = false;
    try {
        engine.updateWhere("records", "data", "hacked", "id", "1");
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        if (msg.find("RLS") != std::string::npos || msg.find("denied") != std::string::npos)
            denied = true;
    }
    if (denied) {
        PASS();
    } else {
        FAIL("UPDATE should have been denied by RLS");
    }
}

void test_rls_delete_enforcement() {
    TEST("RLS blocks DELETE on rows user cannot see");
    milansql::Engine engine;
    engine.createTable("files", {{"id","INT"}, {"owner","TEXT"}, {"name","TEXT"}});
    engine.enableRls("files");

    milansql::Engine::RlsPolicy pol;
    pol.name = "own_only"; pol.table = "files"; pol.command = "ALL";
    pol.role = "PUBLIC"; pol.usingExpr = "owner = CURRENT_USER_ID()";
    engine.createRlsPolicy(pol);

    engine.setCurrentUserDirect("root");
    engine.insertRow("files", {"1", "alice", "doc.txt"});
    engine.insertRow("files", {"2", "bob",   "img.png"});

    // Bob tries to delete alice's file
    engine.setCurrentUserDirect("bob");
    bool denied = false;
    try {
        engine.deleteWhere("files", "id", "1");
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        if (msg.find("RLS") != std::string::npos || msg.find("denied") != std::string::npos)
            denied = true;
    }
    if (denied) {
        PASS();
    } else {
        FAIL("DELETE should have been denied by RLS");
    }
}

void test_rls_with_check_insert() {
    TEST("WITH CHECK blocks INSERT that violates policy");
    milansql::Engine engine;
    engine.createTable("entries", {{"id","INT"}, {"owner","TEXT"}, {"value","TEXT"}});
    engine.enableRls("entries");

    milansql::Engine::RlsPolicy pol;
    pol.name = "own_insert"; pol.table = "entries"; pol.command = "ALL";
    pol.role = "PUBLIC";
    pol.usingExpr = "owner = CURRENT_USER_ID()";
    pol.withCheckExpr = "owner = CURRENT_USER_ID()";
    engine.createRlsPolicy(pol);

    engine.setCurrentUserDirect("alice");
    // Alice inserts her own row — should succeed
    bool insertOk = true;
    try {
        engine.insertRow("entries", {"1", "alice", "my_data"});
    } catch (...) {
        insertOk = false;
    }

    // Alice tries to insert a row as bob — should fail
    bool denied = false;
    try {
        engine.insertRow("entries", {"2", "bob", "fake"});
    } catch (const std::runtime_error& e) {
        std::string msg = e.what();
        if (msg.find("RLS") != std::string::npos || msg.find("WITH CHECK") != std::string::npos)
            denied = true;
    }

    if (insertOk && denied) {
        PASS();
    } else {
        FAIL("Own insert=" + std::to_string(insertOk) + " fake denied=" + std::to_string(denied));
    }
}

void test_rls_root_bypass() {
    TEST("Root bypasses all RLS policies");
    milansql::Engine engine;
    engine.createTable("secrets", {{"id","INT"}, {"owner","TEXT"}});
    engine.enableRls("secrets");

    milansql::Engine::RlsPolicy pol;
    pol.name = "deny_all"; pol.table = "secrets"; pol.command = "ALL";
    pol.role = "PUBLIC"; pol.usingExpr = "1 = 0";  // deny everything
    engine.createRlsPolicy(pol);

    engine.setCurrentUserDirect("root");
    engine.insertRow("secrets", {"1", "admin"});
    auto result = engine.selectAllFiltered("secrets");
    if (result.rows().size() == 1) {
        PASS();
    } else {
        FAIL("Root should bypass RLS");
    }
}

void test_rls_per_command_policies() {
    TEST("Per-command policies (SELECT vs INSERT)");
    milansql::Engine engine;
    engine.createTable("audit", {{"id","INT"}, {"user","TEXT"}, {"action","TEXT"}});
    engine.enableRls("audit");

    // SELECT policy: can only see own rows
    milansql::Engine::RlsPolicy selPol;
    selPol.name = "sel_own"; selPol.table = "audit"; selPol.command = "SELECT";
    selPol.role = "PUBLIC"; selPol.usingExpr = "user = CURRENT_USER_ID()";
    engine.createRlsPolicy(selPol);

    // INSERT policy: can insert anything (1=1)
    milansql::Engine::RlsPolicy insPol;
    insPol.name = "ins_all"; insPol.table = "audit"; insPol.command = "INSERT";
    insPol.role = "PUBLIC"; insPol.usingExpr = "1 = 1";
    engine.createRlsPolicy(insPol);

    engine.setCurrentUserDirect("alice");
    engine.insertRow("audit", {"1", "alice", "login"});
    engine.insertRow("audit", {"2", "bob", "error"});  // alice logs bob's error

    auto result = engine.selectAllFiltered("audit");
    // alice can only SELECT her own row
    if (result.rows().size() == 1 && result.rows()[0].values[1] == "alice") {
        PASS();
    } else {
        FAIL("Expected 1 alice row, got " + std::to_string(result.rows().size()));
    }
}

void test_rls_policy_save_load() {
    TEST("RLS policy save/load preserves withCheckExpr");
    milansql::Engine engine;
    engine.createTable("tbl", {{"id","INT"}, {"x","TEXT"}});
    engine.enableRls("tbl");

    milansql::Engine::RlsPolicy pol;
    pol.name = "p1"; pol.table = "tbl"; pol.command = "ALL";
    pol.role = "PUBLIC"; pol.usingExpr = "x = CURRENT_USER_ID()";
    pol.withCheckExpr = "x != 'admin'";
    engine.createRlsPolicy(pol);

    engine.saveRls("/tmp/test_rls_v2.dat");

    // Read the file and verify withCheckExpr was saved
    std::ifstream rlsFile("/tmp/test_rls_v2.dat");
    std::string fileContent((std::istreambuf_iterator<char>(rlsFile)),
                             std::istreambuf_iterator<char>());
    rlsFile.close();

    bool hasWithCheck = fileContent.find("x != 'admin'") != std::string::npos;
    bool hasUsing = fileContent.find("x = CURRENT_USER_ID()") != std::string::npos;

    if (hasWithCheck && hasUsing) {
        PASS();
    } else {
        FAIL("withCheck=" + std::to_string(hasWithCheck) + " using=" + std::to_string(hasUsing));
    }
}

int main() {
    std::cout << "\n=== Phase 170: Advanced RLS Tests ===\n\n";

    test_rls_and_or();
    test_rls_or();
    test_rls_comparison_operators();
    test_rls_not_equal();
    test_rls_in_operator();
    test_rls_current_user_id();
    test_rls_update_enforcement();
    test_rls_delete_enforcement();
    test_rls_with_check_insert();
    test_rls_root_bypass();
    test_rls_per_command_policies();
    test_rls_policy_save_load();

    std::cout << "\n=== Results: " << passed << " passed, "
              << failed << " failed ===\n\n";
    return failed > 0 ? 1 : 0;
}
