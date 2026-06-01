//! MilanSQL Rust Client
//!
//! Pure Rust client for MilanSQL database with zero external dependencies.
//!
//! # Example
//!
//! ```rust,no_run
//! use milansql::Connection;
//!
//! fn main() -> Result<(), Box<dyn std::error::Error>> {
//!     let mut conn = Connection::connect("localhost:4406")?;
//!     conn.execute("CREATE TABLE test (id INT, name TEXT)")?;
//!     conn.execute("INSERT INTO test VALUES (1, 'Alice')")?;
//!     let result = conn.query("SELECT * FROM test")?;
//!     for row in &result.rows {
//!         println!("{}: {}", row.get_i32("id")?, row.get_string("name")?);
//!     }
//!     Ok(())
//! }
//! ```

pub mod connection;
pub mod error;
pub mod query;
pub mod row;

pub use connection::Connection;
pub use error::MilanSQLError;
pub use query::QueryResult;
pub use row::Row;
