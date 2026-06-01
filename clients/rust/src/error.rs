use std::fmt;
use std::io;

#[derive(Debug)]
pub enum MilanSQLError {
    Io(io::Error),
    Protocol(String),
    NotFound(String),
    TypeMismatch { column: String, expected: &'static str },
}

impl fmt::Display for MilanSQLError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            MilanSQLError::Io(e) => write!(f, "IO error: {}", e),
            MilanSQLError::Protocol(s) => write!(f, "Protocol error: {}", s),
            MilanSQLError::NotFound(s) => write!(f, "Not found: {}", s),
            MilanSQLError::TypeMismatch { column, expected } => {
                write!(f, "Type mismatch for column '{}': expected {}", column, expected)
            }
        }
    }
}

impl std::error::Error for MilanSQLError {}

impl From<io::Error> for MilanSQLError {
    fn from(e: io::Error) -> Self {
        MilanSQLError::Io(e)
    }
}
