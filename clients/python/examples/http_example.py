"""
MilanSQL Python Client — HTTP REST API example

Requires: MilanSQL HTTP server running on port 8080
  .\\build\\milansql.exe --http --port 8080
"""

import milansql
from milansql import MilanSQLError

with milansql.connect_http(host='localhost', port=8080) as conn:

    # ── Server status ─────────────────────────────────────────────
    status = conn.status()
    print("Server Status:")
    for k, v in status.items():
        print(f"  {k}: {v}")

    # ── DDL via HTTP ──────────────────────────────────────────────
    conn.query(
        "CREATE TABLE IF NOT EXISTS users "
        "(id INT PRIMARY KEY AUTO_INCREMENT, name TEXT, score INT)"
    )
    print("\nTabelle erstellt.")

    # ── INSERT ────────────────────────────────────────────────────
    for name, score in [("Alice", 95), ("Bob", 82), ("Charlie", 70)]:
        result = conn.query(f"INSERT INTO users VALUES (NULL, {name!r}, {score})")
        print(f"  INSERT: {result.message or 'OK'}")

    # ── SELECT ────────────────────────────────────────────────────
    result = conn.query("SELECT * FROM users ORDER BY score DESC")
    print(f"\nAlle Benutzer ({result.row_count} Zeilen):")
    print(f"  Spalten: {result.columns}")
    for row in result.rows:
        print(f"  {row}")

    # ── GET via query string ──────────────────────────────────────
    result = conn.query_get("SELECT name, score FROM users WHERE score > 80")
    print(f"\nBenutzer mit Score > 80:")
    for row in result.rows:
        print(f"  {row}")

    # ── Metadata ──────────────────────────────────────────────────
    tables = conn.tables()
    print(f"\nTabellen: {tables}")

    columns = conn.describe("users")
    print(f"DESCRIBE users:")
    for col in columns:
        print(f"  {col}")

    schemas = conn.schemas()
    print(f"\nSchemas: {schemas}")
