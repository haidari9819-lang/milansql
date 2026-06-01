# milansql — Python Client

![PyPI](https://img.shields.io/pypi/v/milansql)
![Python](https://img.shields.io/pypi/pyversions/milansql)

DB-API 2.0 (PEP 249) compliant Python client for MilanSQL database.

## Install

```bash
pip install milansql
```

## Usage

```python
import milansql

conn = milansql.connect(host='localhost', port=4406)
cur = conn.cursor()
cur.execute("CREATE TABLE users (id INT, name TEXT)")
cur.execute("INSERT INTO users VALUES (1, 'Alice')")
cur.execute("SELECT * FROM users")
print(cur.fetchall())
conn.close()
```
