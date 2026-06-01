use crate::error::MilanSQLError;

#[derive(Debug, Clone)]
pub struct Row {
    pub columns: Vec<String>,
    pub values: Vec<Option<String>>,
}

impl Row {
    pub fn new(columns: Vec<String>, values: Vec<Option<String>>) -> Self {
        Row { columns, values }
    }

    pub fn get_raw(&self, col: &str) -> Result<Option<&str>, MilanSQLError> {
        let idx = self.columns.iter().position(|c| c.eq_ignore_ascii_case(col))
            .ok_or_else(|| MilanSQLError::NotFound(format!("Column '{}' not found", col)))?;
        Ok(self.values.get(idx).and_then(|v| v.as_deref()))
    }

    pub fn get_string(&self, col: &str) -> Result<String, MilanSQLError> {
        Ok(self.get_raw(col)?.unwrap_or("").to_string())
    }

    pub fn get_i32(&self, col: &str) -> Result<i32, MilanSQLError> {
        let v = self.get_raw(col)?.unwrap_or("0");
        v.trim().parse().map_err(|_| MilanSQLError::TypeMismatch {
            column: col.to_string(), expected: "i32"
        })
    }

    pub fn get_i64(&self, col: &str) -> Result<i64, MilanSQLError> {
        let v = self.get_raw(col)?.unwrap_or("0");
        v.trim().parse().map_err(|_| MilanSQLError::TypeMismatch {
            column: col.to_string(), expected: "i64"
        })
    }

    pub fn get_f64(&self, col: &str) -> Result<f64, MilanSQLError> {
        let v = self.get_raw(col)?.unwrap_or("0");
        v.trim().parse().map_err(|_| MilanSQLError::TypeMismatch {
            column: col.to_string(), expected: "f64"
        })
    }

    pub fn is_null(&self, col: &str) -> Result<bool, MilanSQLError> {
        Ok(self.get_raw(col)?.is_none())
    }

    pub fn column_count(&self) -> usize {
        self.columns.len()
    }
}
