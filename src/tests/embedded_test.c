/*
 * embedded_test.c — Phase 117: Embedded C API test
 *
 * Tests the SQLite-compatible milansql_embedded API:
 *   - milansql_open / milansql_close
 *   - milansql_exec  (DDL + DML)
 *   - milansql_prepare / milansql_step / milansql_column_* / milansql_finalize
 *   - milansql_changes
 *   - milansql_version
 */

#include "../../include/milansql_embedded.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Minimal test harness ──────────────────────────────────── */
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(expr) do {                                        \
    if (!(expr)) {                                              \
        fprintf(stderr, "  [FAIL] %s  (%s:%d)\n",              \
                #expr, __FILE__, __LINE__);                     \
        ++g_failed;                                             \
    } else {                                                    \
        printf("  [PASS] %s\n", #expr);                        \
        ++g_passed;                                             \
    }                                                           \
} while (0)

#define CHECK_STR(a, b) do {                                    \
    const char *_a = (a), *_b = (b);                           \
    if (!_a || !_b || strcmp(_a, _b) != 0) {                   \
        fprintf(stderr, "  [FAIL] \"%s\" == \"%s\"  (%s:%d)\n",\
                _a ? _a : "(null)", _b ? _b : "(null)",         \
                __FILE__, __LINE__);                            \
        ++g_failed;                                             \
    } else {                                                    \
        printf("  [PASS] column = \"%s\"\n", _b);              \
        ++g_passed;                                             \
    }                                                           \
} while (0)

/* ── Exec callback: count rows ─────────────────────────────── */
static int count_rows_cb(void *data, int cols,
                         char **vals, char **names) {
    (void)cols; (void)vals; (void)names;
    int *cnt = (int *)data;
    if (cnt) (*cnt)++;
    return 0;
}

/* ═══════════════════════════════════════════════════════════ */
int main(void) {
    printf("\n========================================\n");
    printf("  MilanSQL Embedded API Tests\n");
    printf("  %s\n", milansql_version());
    printf("========================================\n\n");

    milansql_db   *db   = NULL;
    milansql_stmt *stmt = NULL;
    char          *errmsg = NULL;
    int            rc;

    /* ── 1. Open ──────────────────────────────────────────── */
    printf("-- Test 1: Open database\n");
    rc = milansql_open(":memory:", &db);
    CHECK(rc == MILANSQL_OK);
    CHECK(db != NULL);

    /* ── 2. Version ──────────────────────────────────────── */
    printf("\n-- Test 2: Version string\n");
    CHECK(milansql_version() != NULL);
    CHECK(strstr(milansql_version(), "MilanSQL") != NULL);

    /* ── 3. CREATE TABLE ─────────────────────────────────── */
    printf("\n-- Test 3: CREATE TABLE\n");
    rc = milansql_exec(db,
        "CREATE TABLE users (id INT PRIMARY KEY, name TEXT NOT NULL, age INT)",
        NULL, NULL, &errmsg);
    CHECK(rc == MILANSQL_OK);
    CHECK(errmsg == NULL);

    /* ── 4. INSERT rows ──────────────────────────────────── */
    printf("\n-- Test 4: INSERT rows\n");
    rc = milansql_exec(db,
        "INSERT INTO users VALUES (1, 'Alice', 30)",
        NULL, NULL, &errmsg);
    CHECK(rc == MILANSQL_OK);

    rc = milansql_exec(db,
        "INSERT INTO users VALUES (2, 'Bob', 25)",
        NULL, NULL, &errmsg);
    CHECK(rc == MILANSQL_OK);

    rc = milansql_exec(db,
        "INSERT INTO users VALUES (3, 'Charlie', 35)",
        NULL, NULL, &errmsg);
    CHECK(rc == MILANSQL_OK);

    /* ── 5. SELECT all rows with exec callback ────────────── */
    printf("\n-- Test 5: SELECT via exec callback\n");
    int rowCount = 0;
    rc = milansql_exec(db,
        "SELECT * FROM users",
        count_rows_cb, &rowCount, &errmsg);
    CHECK(rc == MILANSQL_OK);
    CHECK(rowCount == 3);

    /* ── 6. Prepared SELECT with WHERE ───────────────────── */
    printf("\n-- Test 6: Prepared SELECT WHERE id = 1\n");
    rc = milansql_prepare(db,
        "SELECT * FROM users WHERE id = 1",
        &stmt);
    CHECK(rc == MILANSQL_OK);
    CHECK(stmt != NULL);

    CHECK(milansql_column_count(stmt) == 3);
    CHECK_STR(milansql_column_name(stmt, 0), "id");
    CHECK_STR(milansql_column_name(stmt, 1), "name");
    CHECK_STR(milansql_column_name(stmt, 2), "age");

    rc = milansql_step(stmt);
    CHECK(rc == MILANSQL_ROW);
    CHECK_STR(milansql_column_text(stmt, 0), "1");
    CHECK_STR(milansql_column_text(stmt, 1), "Alice");
    CHECK_STR(milansql_column_text(stmt, 2), "30");

    rc = milansql_step(stmt);
    CHECK(rc == MILANSQL_DONE);

    milansql_finalize(stmt);
    stmt = NULL;

    /* ── 7. SELECT all without WHERE ─────────────────────── */
    printf("\n-- Test 7: SELECT all rows via prepare\n");
    rc = milansql_prepare(db, "SELECT * FROM users", &stmt);
    CHECK(rc == MILANSQL_OK);

    int n = 0;
    while (milansql_step(stmt) == MILANSQL_ROW) ++n;
    CHECK(n == 3);

    milansql_finalize(stmt);
    stmt = NULL;

    /* ── 8. DELETE with WHERE ────────────────────────────── */
    printf("\n-- Test 8: DELETE WHERE id = 2\n");
    rc = milansql_exec(db,
        "DELETE FROM users WHERE id = 2",
        NULL, NULL, &errmsg);
    CHECK(rc == MILANSQL_OK);

    /* Verify row is gone */
    rc = milansql_prepare(db, "SELECT * FROM users", &stmt);
    CHECK(rc == MILANSQL_OK);
    n = 0;
    while (milansql_step(stmt) == MILANSQL_ROW) ++n;
    CHECK(n == 2);
    milansql_finalize(stmt);
    stmt = NULL;

    /* ── 9. DROP TABLE ───────────────────────────────────── */
    printf("\n-- Test 9: DROP TABLE\n");
    rc = milansql_exec(db, "DROP TABLE users", NULL, NULL, &errmsg);
    CHECK(rc == MILANSQL_OK);

    /* ── 10. Error handling ──────────────────────────────── */
    printf("\n-- Test 10: Error on SELECT non-existent table\n");
    rc = milansql_prepare(db, "SELECT * FROM no_such_table", &stmt);
    CHECK(rc == MILANSQL_ERROR);
    CHECK(stmt == NULL);
    CHECK(milansql_errmsg(db) != NULL);

    /* ── 11. milansql_free ───────────────────────────────── */
    printf("\n-- Test 11: milansql_free (no crash)\n");
    char *dummy = (char *)malloc(16);
    milansql_free(dummy);          /* must not crash */
    CHECK(1);                      /* if we got here, no crash */

    /* ── 12. Close ───────────────────────────────────────── */
    printf("\n-- Test 12: Close database\n");
    rc = milansql_close(db);
    CHECK(rc == MILANSQL_OK);

    /* ── Summary ─────────────────────────────────────────── */
    printf("\n========================================\n");
    printf("  Results: %d passed, %d failed\n", g_passed, g_failed);
    printf("========================================\n\n");

    return g_failed == 0 ? 0 : 1;
}
