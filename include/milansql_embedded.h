/*
 * milansql_embedded.h — Phase 117: Embedded Mode (SQLite-compatible C API)
 *
 * Usage:
 *   milansql_db *db;
 *   milansql_open(":memory:", &db);
 *   milansql_exec(db, "CREATE TABLE t (id INT, name TEXT)", NULL, NULL, NULL);
 *   milansql_exec(db, "INSERT INTO t VALUES (1, 'Alice')", NULL, NULL, NULL);
 *
 *   milansql_stmt *stmt;
 *   milansql_prepare(db, "SELECT * FROM t", &stmt);
 *   while (milansql_step(stmt) == MILANSQL_ROW) {
 *       printf("%s  %s\n", milansql_column_text(stmt, 0),
 *                          milansql_column_text(stmt, 1));
 *   }
 *   milansql_finalize(stmt);
 *   milansql_close(db);
 */

#ifndef MILANSQL_EMBEDDED_H
#define MILANSQL_EMBEDDED_H

#ifdef __cplusplus
extern "C" {
#endif

/* ── Return codes ───────────────────────────────────────────── */
#define MILANSQL_OK     0
#define MILANSQL_ERROR  1
#define MILANSQL_ROW    100
#define MILANSQL_DONE   101

/* ── Opaque handles ─────────────────────────────────────────── */
typedef struct milansql_db   milansql_db;
typedef struct milansql_stmt milansql_stmt;

/* Callback type for milansql_exec (same signature as sqlite3_exec) */
typedef int (*milansql_exec_callback)(void *data,
                                      int   colCount,
                                      char **colValues,
                                      char **colNames);

/* ── Connection management ──────────────────────────────────── */
int milansql_open(const char *filename, milansql_db **ppDb);
int milansql_close(milansql_db *db);

/* ── One-step execution ─────────────────────────────────────── */
int milansql_exec(milansql_db             *db,
                  const char              *sql,
                  milansql_exec_callback   callback,
                  void                    *callbackData,
                  char                   **errmsg);

/* ── Prepared-statement API ─────────────────────────────────── */
int milansql_prepare(milansql_db   *db,
                     const char    *sql,
                     milansql_stmt **ppStmt);
int         milansql_step(milansql_stmt *stmt);
int         milansql_column_count(milansql_stmt *stmt);
const char *milansql_column_name(milansql_stmt *stmt, int col);
const char *milansql_column_text(milansql_stmt *stmt, int col);
int         milansql_finalize(milansql_stmt *stmt);

/* ── Diagnostics ────────────────────────────────────────────── */
const char *milansql_errmsg(milansql_db *db);
int         milansql_changes(milansql_db *db);   /* rows affected by last DML */

/* ── Memory ─────────────────────────────────────────────────── */
void milansql_free(void *ptr);

/* ── Version ─────────────────────────────────────────────────── */
const char *milansql_version(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MILANSQL_EMBEDDED_H */
