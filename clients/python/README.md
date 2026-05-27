# milansql — Python Client

Python client library for [MilanSQL](https://github.com/haidari9819-lang/milansql).

- **TCP client** — DB-API 2.0 (PEP 249) compatible, connects to `--server` mode (port 4406)
- **HTTP client** — REST API client, connects to `--http` mode (port 8080)
- No external dependencies — stdlib only (`socket`, `json`, `urllib`)
- Python 3.8+

## Installation

```bash
pip install -e .
```

## TCP Client (DB-API 2.0)

```python
import milansql

with milansql.connect(host='localhost', port=4406) as conn:
    cur = conn.cursor()

    cur.execute("CREATE TABLE users (id INT PRIMARY KEY AUTO_INCREMENT, name TEXT, score INT)")

    # Parameterized queries (%s placeholders)
    cur.execute("INSERT INTO users VALUES (NULL, %s, %s)", ("Alice", 100))
    cur.execute("INSERT INTO users VALUES (NULL, %s, %s)", ("Bob", 200))

    cur.execute("SELECT * FROM users WHERE score > %s", (50,))
    print(cur.description)   # [('id', ...), ('name', ...), ('score', ...)]
    for row in cur.fetchall():
        print(row)           # (1, 'Alice', 100)
```

### Transactions

```python
with milansql.connect() as conn:
    conn.begin()
    cur = conn.cursor()
    cur.execute("UPDATE accounts SET balance = balance - 100 WHERE id = 1")
    cur.execute("UPDATE accounts SET balance = balance + 100 WHERE id = 2")
    conn.commit()   # or conn.rollback()
```

### Cursor API

| Method | Description |
|--------|-------------|
| `cursor.execute(sql, params=None)` | Execute SQL, optionally with `%s` parameters |
| `cursor.executemany(sql, seq)` | Execute SQL for each set of params |
| `cursor.fetchone()` | Return next row as tuple, or `None` |
| `cursor.fetchall()` | Return all rows as list of tuples |
| `cursor.fetchmany(size)` | Return up to `size` rows |
| `cursor.description` | List of `(name, ...)` 7-tuples (columns) |
| `cursor.rowcount` | Number of rows returned or affected |

## HTTP Client (REST API)

```python
import milansql

with milansql.connect_http(host='localhost', port=8080) as conn:
    result = conn.query("SELECT * FROM users")
    print(result.columns)    # ['id', 'name', 'score']
    print(result.rows)       # [[1, 'Alice', 100], [2, 'Bob', 200]]
    print(result.row_count)  # 2

    # Metadata
    print(conn.tables())     # ['users', 'produkte', ...]
    print(conn.schemas())    # ['public']
    print(conn.status())     # {'version': 'MilanSQL v0.9.0', ...}
```

### HttpConnection API

| Method | Description |
|--------|-------------|
| `conn.query(sql)` | POST /query → QueryResult |
| `conn.query_get(sql)` | GET /query?sql=... → QueryResult |
| `conn.tables()` | GET /tables → list of table names |
| `conn.describe(name)` | GET /tables/:name → column info |
| `conn.schemas()` | GET /schemas → list of schemas |
| `conn.status()` | GET /status → server info dict |

### QueryResult fields

| Field | Type | Description |
|-------|------|-------------|
| `result.columns` | `list[str]` | Column names (SELECT) |
| `result.rows` | `list[list]` | Data rows (SELECT) |
| `result.row_count` | `int` | Number of rows |
| `result.rows_affected` | `int` | Rows changed (INSERT/UPDATE/DELETE) |
| `result.message` | `str` | Server message |
| `result.execution_time` | `str` | e.g. `"0.3ms"` |

## Exceptions

```python
from milansql import MilanSQLError, ConnectionError, OperationalError, ProgrammingError

try:
    cur.execute("SELECT * FROM nonexistent")
except MilanSQLError as e:
    print(f"SQL error: {e}")
except milansql.ConnectionError as e:
    print(f"Connection failed: {e}")
```

## Starting the Server

```powershell
# TCP server (port 4406)
.\build\milansql.exe --server --port 4406

# HTTP server (port 8080)
.\build\milansql.exe --http --port 8080

# Both simultaneously
.\build\milansql.exe --server --port 4406 --http --port 8080
```

## Examples

```bash
python examples/basic.py          # TCP basic usage
python examples/transactions.py   # TCP transactions
python examples/http_example.py   # HTTP REST API
```
