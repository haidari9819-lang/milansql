package io.milansql;

import java.io.*;
import java.net.Socket;
import java.sql.*;
import java.util.*;
import java.util.concurrent.Executor;

public class MilanSQLConnection implements Connection {
    private Socket socket;
    private BufferedReader reader;
    private PrintWriter writer;
    private boolean autoCommit = true;
    private boolean closed = false;
    private final String host, database, user;
    private final int port;

    public MilanSQLConnection(String host, int port, String database, String user)
            throws SQLException {
        this.host = host; this.port = port;
        this.database = database; this.user = user;
        try {
            socket = new Socket(host, port);
            reader = new BufferedReader(new InputStreamReader(socket.getInputStream()));
            writer = new PrintWriter(new OutputStreamWriter(socket.getOutputStream()), true);
        } catch (IOException e) {
            throw new SQLException("Cannot connect to " + host + ":" + port + ": " + e.getMessage());
        }
    }

    // Package-private: execute raw SQL, return response lines
    String[] sendQuery(String sql) throws SQLException {
        if (closed) throw new SQLException("Connection is closed");
        writer.println("SQL_QUERY");
        writer.println(sql);
        writer.println("END");
        writer.flush();
        List<String> lines = new ArrayList<>();
        try {
            String line;
            while ((line = reader.readLine()) != null) {
                if ("END".equals(line)) break;
                lines.add(line);
            }
        } catch (IOException e) {
            throw new SQLException("IO error reading response: " + e.getMessage());
        }
        return lines.toArray(new String[0]);
    }

    @Override
    public Statement createStatement() throws SQLException {
        if (closed) throw new SQLException("Connection closed");
        return new MilanSQLStatement(this);
    }

    @Override
    public PreparedStatement prepareStatement(String sql) throws SQLException {
        return new MilanSQLPreparedStatement(this, sql);
    }

    @Override
    public void setAutoCommit(boolean autoCommit) throws SQLException {
        this.autoCommit = autoCommit;
        if (!autoCommit) sendQuery("BEGIN");
    }

    @Override public boolean getAutoCommit() { return autoCommit; }

    @Override
    public void commit() throws SQLException {
        sendQuery("COMMIT");
    }

    @Override
    public void rollback() throws SQLException {
        sendQuery("ROLLBACK");
    }

    @Override
    public void close() throws SQLException {
        if (!closed) {
            closed = true;
            try { socket.close(); } catch (IOException e) { /* ignore */ }
        }
    }

    @Override public boolean isClosed() { return closed; }
    @Override public DatabaseMetaData getMetaData() throws SQLException {
        return new MilanSQLDatabaseMetaData(this, host, port, database);
    }

    // Minimal stub implementations for unused Connection methods
    @Override public void setReadOnly(boolean r) {}
    @Override public boolean isReadOnly() { return false; }
    @Override public void setCatalog(String c) {}
    @Override public String getCatalog() { return database; }
    @Override public void setTransactionIsolation(int l) {}
    @Override public int getTransactionIsolation() { return TRANSACTION_READ_COMMITTED; }
    @Override public SQLWarning getWarnings() { return null; }
    @Override public void clearWarnings() {}
    @Override public Statement createStatement(int t, int c) throws SQLException { return createStatement(); }
    @Override public PreparedStatement prepareStatement(String sql, int t, int c) throws SQLException { return prepareStatement(sql); }
    @Override public CallableStatement prepareCall(String sql) throws SQLException { throw new SQLFeatureNotSupportedException("prepareCall"); }
    @Override public CallableStatement prepareCall(String sql, int t, int c) throws SQLException { throw new SQLFeatureNotSupportedException("prepareCall"); }
    @Override public String nativeSQL(String sql) { return sql; }
    @Override public Map<String,Class<?>> getTypeMap() { return new HashMap<>(); }
    @Override public void setTypeMap(Map<String,Class<?>> m) {}
    @Override public void setHoldability(int h) {}
    @Override public int getHoldability() { return ResultSet.HOLD_CURSORS_OVER_COMMIT; }
    @Override public Savepoint setSavepoint() throws SQLException { sendQuery("SAVEPOINT sp1"); return null; }
    @Override public Savepoint setSavepoint(String name) throws SQLException { sendQuery("SAVEPOINT " + name); return null; }
    @Override public void rollback(Savepoint sp) throws SQLException { sendQuery("ROLLBACK TO " + (sp != null ? sp.getSavepointName() : "sp1")); }
    @Override public void releaseSavepoint(Savepoint sp) {}
    @Override public Statement createStatement(int t, int c, int h) throws SQLException { return createStatement(); }
    @Override public PreparedStatement prepareStatement(String sql, int t, int c, int h) throws SQLException { return prepareStatement(sql); }
    @Override public PreparedStatement prepareStatement(String sql, int[] cols) throws SQLException { return prepareStatement(sql); }
    @Override public PreparedStatement prepareStatement(String sql, String[] cols) throws SQLException { return prepareStatement(sql); }
    @Override public PreparedStatement prepareStatement(String sql, int flag) throws SQLException { return prepareStatement(sql); }
    @Override public CallableStatement prepareCall(String sql, int t, int c, int h) throws SQLException { throw new SQLFeatureNotSupportedException(); }
    @Override public Clob createClob() throws SQLException { throw new SQLFeatureNotSupportedException(); }
    @Override public Blob createBlob() throws SQLException { throw new SQLFeatureNotSupportedException(); }
    @Override public NClob createNClob() throws SQLException { throw new SQLFeatureNotSupportedException(); }
    @Override public SQLXML createSQLXML() throws SQLException { throw new SQLFeatureNotSupportedException(); }
    @Override public boolean isValid(int timeout) { return !closed; }
    @Override public void setClientInfo(String name, String value) {}
    @Override public void setClientInfo(Properties props) {}
    @Override public String getClientInfo(String name) { return null; }
    @Override public Properties getClientInfo() { return new Properties(); }
    @Override public Array createArrayOf(String t, Object[] e) throws SQLException { throw new SQLFeatureNotSupportedException(); }
    @Override public Struct createStruct(String t, Object[] a) throws SQLException { throw new SQLFeatureNotSupportedException(); }
    @Override public void setSchema(String s) {}
    @Override public String getSchema() { return "public"; }
    @Override public void abort(Executor e) {}
    @Override public void setNetworkTimeout(Executor e, int ms) {}
    @Override public int getNetworkTimeout() { return 0; }
    @Override public <T> T unwrap(Class<T> iface) throws SQLException { throw new SQLException("Not a wrapper"); }
    @Override public boolean isWrapperFor(Class<?> iface) { return false; }
}
