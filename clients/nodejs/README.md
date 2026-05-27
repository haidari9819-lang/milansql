# milansql — Node.js Client

Node.js client for [MilanSQL](https://github.com/haidari9819-lang/milansql) via the HTTP REST API.

- Zero dependencies — uses Node.js built-in `http` module
- Node.js 14+
- Connects to MilanSQL `--http` mode (default port 8080)

## Usage

```js
const milansql = require('milansql');

const conn = milansql.connect({ host: 'localhost', port: 8080 });

// Execute SQL (POST /query)
const result = await conn.query('SELECT * FROM users');
console.log(result.columns);   // ['id', 'name', 'score']
console.log(result.rows);      // [[1, 'Alice', 95], [2, 'Bob', 82]]
console.log(result.rowCount);  // 2

// Via GET /query?sql=...
const r2 = await conn.queryGet('SELECT * FROM users WHERE score > 80');

// Metadata
const tables  = await conn.tables();
const columns = await conn.describe('users');
const schemas = await conn.schemas();
const status  = await conn.status();
```

## API

### `milansql.connect(options)` → `Connection`

| Option | Default | Description |
|--------|---------|-------------|
| `host` | `'localhost'` | Server hostname |
| `port` | `8080` | HTTP port |
| `timeout` | `30000` | Request timeout (ms) |

### `Connection`

| Method | Returns | Description |
|--------|---------|-------------|
| `conn.query(sql)` | `Promise<QueryResult>` | POST /query |
| `conn.queryGet(sql)` | `Promise<QueryResult>` | GET /query?sql=... |
| `conn.tables()` | `Promise<string[]>` | All table names |
| `conn.describe(name)` | `Promise<object[]>` | Column info |
| `conn.schemas()` | `Promise<string[]>` | All schema names |
| `conn.status()` | `Promise<object>` | Server info |
| `conn.close()` | `void` | No-op (HTTP is stateless) |

### `QueryResult`

| Field | Type | Description |
|-------|------|-------------|
| `result.columns` | `string[]` | Column names |
| `result.rows` | `any[][]` | Data rows |
| `result.rowCount` | `number` | Row count |
| `result.rowsAffected` | `number` | Rows changed |
| `result.message` | `string` | Server message |
| `result.executionTime` | `string` | e.g. `"0.3ms"` |
| `result.fetchall()` | `any[][]` | Copy of rows |
| `result.fetchone()` | `any[] \| null` | First row |

## Start the Server

```powershell
.\build\milansql.exe --http --port 8080
```

## Run the Example

```bash
node examples/basic.js
```
