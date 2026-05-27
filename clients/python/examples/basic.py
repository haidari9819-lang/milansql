"""
MilanSQL Python Client — Basic TCP example

Requires: MilanSQL server running on port 4406
  .\\build\\milansql.exe --server --port 4406
"""

import milansql

with milansql.connect(host='localhost', port=4406) as conn:
    cur = conn.cursor()

    # Create table
    cur.execute(
        "CREATE TABLE IF NOT EXISTS produkte "
        "(id INT PRIMARY KEY AUTO_INCREMENT, name TEXT, preis INT)"
    )
    print("Tabelle erstellt.")

    # Insert rows with parameterized queries
    cur.execute("INSERT INTO produkte VALUES (NULL, %s, %s)", ("Laptop", 1200))
    cur.execute("INSERT INTO produkte VALUES (NULL, %s, %s)", ("Maus", 25))
    cur.execute("INSERT INTO produkte VALUES (NULL, %s, %s)", ("Monitor", 350))
    print("3 Zeilen eingefügt.")

    # SELECT with filter
    cur.execute("SELECT * FROM produkte WHERE preis > %s ORDER BY preis DESC", (100,))
    print(f"\nProdukte teurer als 100 ({cur.rowcount} Ergebnisse):")
    print(f"  Spalten: {[col[0] for col in cur.description]}")
    for row in cur.fetchall():
        print(f"  {row}")

    # Aggregate
    cur.execute("SELECT COUNT(*), AVG(preis), MAX(preis) FROM produkte")
    row = cur.fetchone()
    print(f"\nStatistik: Anzahl={row[0]}, Durchschnitt={row[1]}, Maximum={row[2]}")
