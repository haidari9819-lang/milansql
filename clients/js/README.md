# milansql-js

JavaScript client for [MilanSQL](https://milansql.de) — the open source, self-hosted database for developers.

## Install

```bash
npm install milansql
```

Or use directly in the browser:
```html
<script src="https://milansql.de/sdk/milansql.js"></script>
```

## Usage

```js
import MilanSQL from 'milansql';

const db = new MilanSQL('https://milansql.de', 'your-jwt-token');

// Raw SQL
const result = await db.query('SELECT version()');

// Supabase-style query builder
const { rows } = await db
  .from('products')
  .select('name, price')
  .eq('category', 'IT')
  .gt('price', 100)
  .order('price', { ascending: false })
  .limit(10)
  .execute();
```

## Auth

```js
// Login (saves token automatically)
const db = new MilanSQL('https://milansql.de');
await db.login('alice', 'password');

// Register
await db.register('alice', 'password');

// Current user
const { username, user_id } = await db.me();

// Logout
await db.logout();
```

## API

### `new MilanSQL(url, token?)`
Create a client instance.

### `db.query(sql)` → `Promise<{success, columns, rows, rowCount}>`
Execute raw SQL.

### `db.from(table)` → `QueryBuilder`
Start a query builder chain.

### `db.tables()` → `Promise<string[]>`
List all tables for the current user.

### `db.health()` → `Promise<object>`
Check server health.

## QueryBuilder Methods

| Method | Description |
|--------|-------------|
| `.select(cols)` | Columns to select (default: `*`) |
| `.eq(col, val)` | WHERE col = val |
| `.neq(col, val)` | WHERE col != val |
| `.gt(col, val)` | WHERE col > val |
| `.gte(col, val)` | WHERE col >= val |
| `.lt(col, val)` | WHERE col < val |
| `.lte(col, val)` | WHERE col <= val |
| `.like(col, pat)` | WHERE col LIKE pat |
| `.order(col, {ascending})` | ORDER BY |
| `.limit(n)` | LIMIT n |
| `.execute()` | Run the query |

## License

MIT
