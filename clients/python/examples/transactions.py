"""
MilanSQL Python Client — Transactions example

Requires: MilanSQL server running on port 4406
  .\\build\\milansql.exe --server --port 4406
"""

import milansql
from milansql import MilanSQLError

conn = milansql.connect(host='localhost', port=4406)

try:
    cur = conn.cursor()

    cur.execute(
        "CREATE TABLE IF NOT EXISTS konten "
        "(id INT PRIMARY KEY, name TEXT, saldo INT)"
    )
    cur.execute("INSERT INTO konten VALUES (1, %s, %s)", ("Alice", 1000))
    cur.execute("INSERT INTO konten VALUES (2, %s, %s)", ("Bob", 500))

    # ── Successful transaction ────────────────────────────────────
    print("Starte Transaktion: 200 von Alice an Bob...")
    conn.begin()
    cur.execute("UPDATE konten SET saldo = saldo - 200 WHERE id = 1")
    cur.execute("UPDATE konten SET saldo = saldo + 200 WHERE id = 2")
    conn.commit()
    print("Commit erfolgreich.")

    cur.execute("SELECT * FROM konten")
    for row in cur.fetchall():
        print(f"  {row}")

    # ── Failed transaction (rollback) ─────────────────────────────
    print("\nStarte Transaktion (wird abgebrochen)...")
    conn.begin()
    cur.execute("UPDATE konten SET saldo = saldo - 999 WHERE id = 1")
    print("  Simuliere Fehler → Rollback")
    conn.rollback()
    print("Rollback erfolgreich.")

    cur.execute("SELECT * FROM konten")
    for row in cur.fetchall():
        print(f"  {row}")

except MilanSQLError as e:
    print(f"Datenbankfehler: {e}")
    conn.rollback()
finally:
    conn.close()
