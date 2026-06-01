use crate::row::Row;

#[derive(Debug)]
pub struct QueryResult {
    pub columns: Vec<String>,
    pub rows: Vec<Row>,
    pub rows_affected: usize,
}

impl QueryResult {
    pub fn new(columns: Vec<String>, rows: Vec<Row>) -> Self {
        let rows_affected = rows.len();
        QueryResult { columns, rows, rows_affected }
    }

    pub fn empty() -> Self {
        QueryResult { columns: vec![], rows: vec![], rows_affected: 0 }
    }

    pub fn row_count(&self) -> usize {
        self.rows.len()
    }

    pub fn is_empty(&self) -> bool {
        self.rows.is_empty()
    }
}

// Parse server response lines into a QueryResult
pub(crate) fn parse_response(lines: &[String]) -> QueryResult {
    let data_lines: Vec<&str> = lines.iter()
        .map(|l| l.as_str())
        .filter(|l| {
            !l.is_empty()
            && !l.starts_with("OK")
            && !l.starts_with("ERROR")
            && !l.starts_with('+')
            && !l.starts_with('├')
            && !l.starts_with('└')
            && !l.starts_with('┌')
            && (l.contains('│') || l.contains('|') || l.contains('\t'))
        })
        .collect();

    if data_lines.is_empty() {
        return QueryResult::empty();
    }

    let columns = split_row(data_lines[0]);
    let mut rows = Vec::new();

    for line in &data_lines[1..] {
        let vals = split_row(line);
        let values: Vec<Option<String>> = vals.iter().map(|v| {
            if v == "NULL" { None } else { Some(v.clone()) }
        }).collect();
        rows.push(Row::new(columns.clone(), values));
    }

    QueryResult::new(columns, rows)
}

fn split_row(line: &str) -> Vec<String> {
    let parts: Vec<&str> = if line.contains('│') {
        line.split('│').collect()
    } else if line.contains('|') {
        line.split('|').collect()
    } else {
        line.split('\t').collect()
    };
    parts.iter()
        .map(|p| p.trim().to_string())
        .filter(|p| !p.is_empty())
        .collect()
}
