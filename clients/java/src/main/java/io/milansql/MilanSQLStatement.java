package io.milansql;

import java.sql.*;

public class MilanSQLStatement implements Statement {
    protected final MilanSQLConnection conn;
    private MilanSQLResultSet currentRs;
    private int updateCount = -1;

    public MilanSQLStatement(MilanSQLConnection conn) { this.conn = conn; }

    @Override
    public ResultSet executeQuery(String sql) throws SQLException {
        String[] lines = conn.sendQuery(sql);
        currentRs = MilanSQLResultSet.parse(lines);
        updateCount = -1;
        return currentRs;
    }

    @Override
    public int executeUpdate(String sql) throws SQLException {
        String[] lines = conn.sendQuery(sql);
        updateCount = 0;
        for (String line : lines) {
            if (line.startsWith("OK")) { updateCount = 1; break; }
            // try parse "N rows affected"
            if (line.matches("\\d+ row.*")) {
                try { updateCount = Integer.parseInt(line.split(" ")[0]); } catch (Exception ignored) {}
                break;
            }
        }
        return updateCount;
    }

    @Override
    public boolean execute(String sql) throws SQLException {
        String[] lines = conn.sendQuery(sql);
        // Heuristic: if first non-empty line looks like a header row with |, it's a SELECT
        for (String line : lines) {
            if (line.trim().isEmpty()) continue;
            if (line.contains("|") || line.contains("\t")) {
                currentRs = MilanSQLResultSet.parse(lines);
                updateCount = -1;
                return true;
            }
            break;
        }
        updateCount = 0;
        currentRs = null;
        return false;
    }

    @Override public ResultSet getResultSet() { return currentRs; }
    @Override public int getUpdateCount() { return updateCount; }
    @Override public boolean getMoreResults() { return false; }
    @Override public void close() {}
    @Override public boolean isClosed() { return false; }
    @Override public Connection getConnection() { return conn; }

    // Stub implementations
    @Override public int getMaxFieldSize() { return 0; }
    @Override public void setMaxFieldSize(int m) {}
    @Override public int getMaxRows() { return 0; }
    @Override public void setMaxRows(int m) {}
    @Override public void setEscapeProcessing(boolean e) {}
    @Override public int getQueryTimeout() { return 0; }
    @Override public void setQueryTimeout(int s) {}
    @Override public void cancel() {}
    @Override public SQLWarning getWarnings() { return null; }
    @Override public void clearWarnings() {}
    @Override public void setCursorName(String n) {}
    @Override public ResultSet getGeneratedKeys() throws SQLException { return MilanSQLResultSet.empty(); }
    @Override public boolean getMoreResults(int c) { return false; }
    @Override public int[] executeBatch() throws SQLException { return new int[0]; }
    @Override public void addBatch(String sql) {}
    @Override public void clearBatch() {}
    @Override public int getFetchDirection() { return ResultSet.FETCH_FORWARD; }
    @Override public void setFetchDirection(int d) {}
    @Override public int getFetchSize() { return 0; }
    @Override public void setFetchSize(int r) {}
    @Override public int getResultSetConcurrency() { return ResultSet.CONCUR_READ_ONLY; }
    @Override public int getResultSetType() { return ResultSet.TYPE_FORWARD_ONLY; }
    @Override public int getResultSetHoldability() { return ResultSet.HOLD_CURSORS_OVER_COMMIT; }
    @Override public boolean isPoolable() { return false; }
    @Override public void setPoolable(boolean p) {}
    @Override public void closeOnCompletion() {}
    @Override public boolean isCloseOnCompletion() { return false; }
    @Override public <T> T unwrap(Class<T> i) throws SQLException { throw new SQLException("Not a wrapper"); }
    @Override public boolean isWrapperFor(Class<?> i) { return false; }
    @Override public int executeUpdate(String sql, int flag) throws SQLException { return executeUpdate(sql); }
    @Override public int executeUpdate(String sql, int[] cols) throws SQLException { return executeUpdate(sql); }
    @Override public int executeUpdate(String sql, String[] cols) throws SQLException { return executeUpdate(sql); }
    @Override public boolean execute(String sql, int flag) throws SQLException { return execute(sql); }
    @Override public boolean execute(String sql, int[] cols) throws SQLException { return execute(sql); }
    @Override public boolean execute(String sql, String[] cols) throws SQLException { return execute(sql); }
}
