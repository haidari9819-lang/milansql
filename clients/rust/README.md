# milansql — Rust Client

Pure Rust client for [MilanSQL](https://github.com/haidari9819-lang/milansql) database.
Zero external dependencies.

## Installation

```toml
[dependencies]
milansql = "4.2.0"
```

## Usage

```rust
use milansql::Connection;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let mut conn = Connection::connect("localhost:4406")?;
    
    conn.execute("CREATE TABLE users (id INT, name TEXT)")?;
    conn.execute("INSERT INTO users VALUES (1, 'Alice')")?;
    conn.execute("INSERT INTO users VALUES (2, 'Bob')")?;
    
    let result = conn.query("SELECT * FROM users")?;
    println!("Found {} rows:", result.row_count());
    for row in &result.rows {
        println!("  {}: {}", row.get_i32("id")?, row.get_string("name")?);
    }
    
    // Transactions
    conn.begin()?;
    conn.execute("INSERT INTO users VALUES (3, 'Carol')")?;
    conn.commit()?;
    
    Ok(())
}
```

## API

- `Connection::connect(addr)` — connect to MilanSQL server
- `conn.query(sql)` → `QueryResult` — execute SELECT
- `conn.execute(sql)` → `usize` — execute DML/DDL
- `conn.begin()` / `conn.commit()` / `conn.rollback()`
- `row.get_string(col)` / `row.get_i32(col)` / `row.get_f64(col)`
