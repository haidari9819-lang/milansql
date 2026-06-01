package io.milansql;
import java.sql.*;
import java.util.*;
import java.math.BigDecimal;
import java.io.InputStream;
import java.io.Reader;
import java.util.Calendar;

public class MilanSQLPreparedStatement extends MilanSQLStatement implements PreparedStatement {
    private final String sqlTemplate;
    private final Map<Integer, String> params = new HashMap<>();

    public MilanSQLPreparedStatement(MilanSQLConnection conn, String sql) {
        super(conn);
        this.sqlTemplate = sql;
    }

    private String buildSql() {
        String s = sqlTemplate;
        // Replace ? in order
        int idx = 1;
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < s.length(); i++) {
            if (s.charAt(i) == '?') {
                sb.append(params.getOrDefault(idx++, "NULL"));
            } else {
                sb.append(s.charAt(i));
            }
        }
        return sb.toString();
    }

    @Override public ResultSet executeQuery() throws SQLException { return executeQuery(buildSql()); }
    @Override public int executeUpdate() throws SQLException { return executeUpdate(buildSql()); }
    @Override public boolean execute() throws SQLException { return execute(buildSql()); }

    @Override public void setNull(int i, int t) { params.put(i, "NULL"); }
    @Override public void setBoolean(int i, boolean v) { params.put(i, v ? "1" : "0"); }
    @Override public void setByte(int i, byte v) { params.put(i, String.valueOf(v)); }
    @Override public void setShort(int i, short v) { params.put(i, String.valueOf(v)); }
    @Override public void setInt(int i, int v) { params.put(i, String.valueOf(v)); }
    @Override public void setLong(int i, long v) { params.put(i, String.valueOf(v)); }
    @Override public void setFloat(int i, float v) { params.put(i, String.valueOf(v)); }
    @Override public void setDouble(int i, double v) { params.put(i, String.valueOf(v)); }
    @Override public void setBigDecimal(int i, BigDecimal v) { params.put(i, v == null ? "NULL" : v.toPlainString()); }
    @Override public void setString(int i, String v) { params.put(i, v == null ? "NULL" : "'" + v.replace("'", "''") + "'"); }
    @Override public void setObject(int i, Object v) { params.put(i, v == null ? "NULL" : v.toString()); }
    @Override public void setObject(int i, Object v, int t) { setObject(i, v); }
    @Override public void setObject(int i, Object v, int t, int s) { setObject(i, v); }
    @Override public void clearParameters() { params.clear(); }
    @Override public void addBatch() {}

    // Stubs for less common methods
    @Override public void setBytes(int i, byte[] v) { params.put(i, "NULL"); }
    @Override public void setDate(int i, java.sql.Date v) { params.put(i, v == null ? "NULL" : "'" + v + "'"); }
    @Override public void setDate(int i, java.sql.Date v, Calendar c) { setDate(i, v); }
    @Override public void setTime(int i, java.sql.Time v) { params.put(i, v == null ? "NULL" : "'" + v + "'"); }
    @Override public void setTime(int i, java.sql.Time v, Calendar c) { setTime(i, v); }
    @Override public void setTimestamp(int i, java.sql.Timestamp v) { params.put(i, v == null ? "NULL" : "'" + v + "'"); }
    @Override public void setTimestamp(int i, java.sql.Timestamp v, Calendar c) { setTimestamp(i, v); }
    @Override public void setAsciiStream(int i, InputStream v, int l) {}
    @Override public void setUnicodeStream(int i, InputStream v, int l) {}
    @Override public void setBinaryStream(int i, InputStream v, int l) {}
    @Override public void setBinaryStream(int i, InputStream v, long l) {}
    @Override public void setAsciiStream(int i, InputStream v, long l) {}
    @Override public void setAsciiStream(int i, InputStream v) {}
    @Override public void setBinaryStream(int i, InputStream v) {}
    @Override public void setCharacterStream(int i, Reader v, int l) {}
    @Override public void setCharacterStream(int i, Reader v, long l) {}
    @Override public void setCharacterStream(int i, Reader v) {}
    @Override public void setRef(int i, Ref v) {}
    @Override public void setBlob(int i, Blob v) {}
    @Override public void setBlob(int i, InputStream v, long l) {}
    @Override public void setBlob(int i, InputStream v) {}
    @Override public void setClob(int i, Clob v) {}
    @Override public void setClob(int i, Reader v, long l) {}
    @Override public void setClob(int i, Reader v) {}
    @Override public void setArray(int i, Array v) {}
    @Override public ResultSetMetaData getMetaData() { return null; }
    @Override public ParameterMetaData getParameterMetaData() { return null; }
    @Override public void setRowId(int i, java.sql.RowId v) {}
    @Override public void setNString(int i, String v) { setString(i, v); }
    @Override public void setNCharacterStream(int i, Reader v, long l) {}
    @Override public void setNClob(int i, java.sql.NClob v) {}
    @Override public void setNClob(int i, Reader v, long l) {}
    @Override public void setNClob(int i, Reader v) {}
    @Override public void setSQLXML(int i, java.sql.SQLXML v) {}
    @Override public void setNCharacterStream(int i, Reader v) {}
    @Override public void setNull(int i, int t, String n) { params.put(i, "NULL"); }
    @Override public void setURL(int i, java.net.URL v) {}
}
