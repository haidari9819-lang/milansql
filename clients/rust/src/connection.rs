use std::io::{BufRead, BufReader, Write};
use std::net::TcpStream;
use crate::error::MilanSQLError;
use crate::query::{QueryResult, parse_response};

pub struct Connection {
    writer: TcpStream,
    reader: BufReader<TcpStream>,
}

impl Connection {
    /// Connect to a MilanSQL server at "host:port"
    pub fn connect(addr: &str) -> Result<Self, MilanSQLError> {
        let stream = TcpStream::connect(addr)?;
        let reader_stream = stream.try_clone()?;
        Ok(Connection {
            writer: stream,
            reader: BufReader::new(reader_stream),
        })
    }

    /// Send a SQL statement and return the raw response lines
    fn send_raw(&mut self, sql: &str) -> Result<Vec<String>, MilanSQLError> {
        // Protocol: SQL_QUERY\n{sql}\nEND\n
        let msg = format!("SQL_QUERY\n{}\nEND\n", sql);
        self.writer.write_all(msg.as_bytes())?;
        self.writer.flush()?;

        let mut lines = Vec::new();
        loop {
            let mut line = String::new();
            let n = self.reader.read_line(&mut line)?;
            if n == 0 { break; }
            let trimmed = line.trim_end_matches('\n').trim_end_matches('\r').to_string();
            if trimmed == "END" { break; }
            lines.push(trimmed);
        }
        Ok(lines)
    }

    /// Execute a query and return structured results (SELECT)
    pub fn query(&mut self, sql: &str) -> Result<QueryResult, MilanSQLError> {
        let lines = self.send_raw(sql)?;
        // Check for error response
        for line in &lines {
            if line.starts_with("ERROR") {
                return Err(MilanSQLError::Protocol(line.clone()));
            }
        }
        Ok(parse_response(&lines))
    }

    /// Execute a non-query statement (INSERT/UPDATE/DELETE/DDL)
    pub fn execute(&mut self, sql: &str) -> Result<usize, MilanSQLError> {
        let lines = self.send_raw(sql)?;
        for line in &lines {
            if line.starts_with("ERROR") {
                return Err(MilanSQLError::Protocol(line.clone()));
            }
        }
        // Try to parse affected rows from response
        for line in &lines {
            if let Some(n) = line.split_whitespace().next() {
                if let Ok(count) = n.parse::<usize>() {
                    return Ok(count);
                }
            }
        }
        Ok(1)
    }

    /// Begin a transaction
    pub fn begin(&mut self) -> Result<(), MilanSQLError> {
        self.execute("BEGIN")?;
        Ok(())
    }

    /// Commit current transaction
    pub fn commit(&mut self) -> Result<(), MilanSQLError> {
        self.execute("COMMIT")?;
        Ok(())
    }

    /// Rollback current transaction
    pub fn rollback(&mut self) -> Result<(), MilanSQLError> {
        self.execute("ROLLBACK")?;
        Ok(())
    }

    /// Ping the server (send a simple query)
    pub fn ping(&mut self) -> Result<(), MilanSQLError> {
        self.execute("SELECT 1")?;
        Ok(())
    }
}

impl Drop for Connection {
    fn drop(&mut self) {
        // writer and reader streams dropped automatically
    }
}
