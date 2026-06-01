package io.milansql;

import java.sql.*;
import java.util.*;
import java.math.BigDecimal;
import java.io.*;

public class MilanSQLResultSet implements ResultSet {
    private final List<String[]> rows;
    private final String[] columns;
    private int cursor = -1;
    private boolean wasNull = false;

    public MilanSQLResultSet(String[] columns, List<String[]> rows) {
        this.columns = columns;
        this.rows = rows;
    }

    public static MilanSQLResultSet empty() {
        return new MilanSQLResultSet(new String[0], new ArrayList<>());
    }

    // Parse tab-separated or box-drawing format from server response
    public static MilanSQLResultSet parse(String[] lines) {
        List<String> dataLines = new ArrayList<>();
        for (String l : lines) {
            if (l.startsWith("OK") || l.startsWith("ERROR") || l.trim().isEmpty()) continue;
            // Skip box-drawing separator lines like "+---+---+"
            if (l.startsWith("+") || l.startsWith("\u251C") || l.startsWith("\u2514") || l.startsWith("\u250C")) continue;
            if (l.startsWith("\u2502") || l.contains("|")) dataLines.add(l);
            else if (l.contains("\t")) dataLines.add(l);
        }

        if (dataLines.isEmpty()) return empty();

        String[] cols = splitRow(dataLines.get(0));
        List<String[]> rows = new ArrayList<>();
        for (int i = 1; i < dataLines.size(); i++) {
            // Skip separator lines
            String line = dataLines.get(i);
            if (line.startsWith("\u251C") || line.startsWith("\u2500")) continue;
            rows.add(splitRow(line));
        }
        return new MilanSQLResultSet(cols, rows);
    }

    private static String[] splitRow(String line) {
        // Handle box-drawing: \u2502 col1 \u2502 col2 \u2502
        if (line.contains("\u2502")) {
            String[] parts = line.split("\u2502");
            List<String> r = new ArrayList<>();
            for (String p : parts) { String t = p.trim(); if (!t.isEmpty()) r.add(t); }
            return r.toArray(new String[0]);
        }
        if (line.contains("|")) {
            String[] parts = line.split("\\|");
            List<String> r = new ArrayList<>();
            for (String p : parts) { String t = p.trim(); if (!t.isEmpty()) r.add(t); }
            return r.toArray(new String[0]);
        }
        return line.split("\t");
    }

    @Override public boolean next() { cursor++; return cursor < rows.size(); }
    @Override public void close() {}
    @Override public boolean wasNull() { return wasNull; }

    private String get(int col) {
        if (cursor < 0 || cursor >= rows.size()) return null;
        String[] row = rows.get(cursor);
        if (col < 1 || col > row.length) return null;
        String v = row[col - 1];
        wasNull = (v == null || v.equals("NULL"));
        return wasNull ? null : v;
    }

    private String get(String label) {
        for (int i = 0; i < columns.length; i++) {
            if (columns[i].equalsIgnoreCase(label)) return get(i + 1);
        }
        return null;
    }

    @Override public String getString(int col) { return get(col); }
    @Override public String getString(String col) { return get(col); }
    @Override public int getInt(int col) { String v = get(col); return v==null?0:Integer.parseInt(v.trim()); }
    @Override public int getInt(String col) { String v = get(col); return v==null?0:Integer.parseInt(v.trim()); }
    @Override public double getDouble(int col) { String v = get(col); return v==null?0:Double.parseDouble(v.trim()); }
    @Override public double getDouble(String col) { String v = get(col); return v==null?0:Double.parseDouble(v.trim()); }
    @Override public long getLong(int col) { String v = get(col); return v==null?0:Long.parseLong(v.trim()); }
    @Override public long getLong(String col) { String v = get(col); return v==null?0:Long.parseLong(v.trim()); }
    @Override public float getFloat(int col) { return (float)getDouble(col); }
    @Override public float getFloat(String col) { return (float)getDouble(col); }
    @Override public boolean getBoolean(int col) { String v = get(col); return "1".equals(v)||"true".equalsIgnoreCase(v); }
    @Override public boolean getBoolean(String col) { String v = get(col); return "1".equals(v)||"true".equalsIgnoreCase(v); }
    @Override public Object getObject(int col) { return get(col); }
    @Override public Object getObject(String col) { return get(col); }
    @Override public BigDecimal getBigDecimal(int col, int scale) { String v = get(col); return v==null?BigDecimal.ZERO:new BigDecimal(v.trim()).setScale(scale, java.math.RoundingMode.HALF_UP); }
    @Override public BigDecimal getBigDecimal(int col) { String v = get(col); return v==null?null:new BigDecimal(v.trim()); }
    @Override public BigDecimal getBigDecimal(String col) { String v = get(col); return v==null?null:new BigDecimal(v.trim()); }
    @Override public BigDecimal getBigDecimal(String col, int scale) { return getBigDecimal(col).setScale(scale, java.math.RoundingMode.HALF_UP); }

    @Override public ResultSetMetaData getMetaData() { return new SimpleRSMD(columns); }

    // Stubs
    @Override public byte getByte(int c) { return 0; } @Override public byte getByte(String c) { return 0; }
    @Override public short getShort(int c) { return 0; } @Override public short getShort(String c) { return 0; }
    @Override public byte[] getBytes(int c) { return new byte[0]; } @Override public byte[] getBytes(String c) { return new byte[0]; }
    @Override public java.sql.Date getDate(int c) { return null; } @Override public java.sql.Date getDate(String c) { return null; }
    @Override public java.sql.Time getTime(int c) { return null; } @Override public java.sql.Time getTime(String c) { return null; }
    @Override public java.sql.Timestamp getTimestamp(int c) { return null; } @Override public java.sql.Timestamp getTimestamp(String c) { return null; }
    @Override public InputStream getAsciiStream(int c) { return null; } @Override public InputStream getAsciiStream(String c) { return null; }
    @Override public InputStream getUnicodeStream(int c) { return null; } @Override public InputStream getUnicodeStream(String c) { return null; }
    @Override public InputStream getBinaryStream(int c) { return null; } @Override public InputStream getBinaryStream(String c) { return null; }
    @Override public SQLWarning getWarnings() { return null; } @Override public void clearWarnings() {}
    @Override public String getCursorName() { return "cursor"; }
    @Override public Reader getCharacterStream(int c) { return null; } @Override public Reader getCharacterStream(String c) { return null; }
    @Override public boolean isBeforeFirst() { return cursor < 0; }
    @Override public boolean isAfterLast() { return cursor >= rows.size(); }
    @Override public boolean isFirst() { return cursor == 0; }
    @Override public boolean isLast() { return cursor == rows.size() - 1; }
    @Override public void beforeFirst() { cursor = -1; }
    @Override public void afterLast() { cursor = rows.size(); }
    @Override public boolean first() { cursor = 0; return !rows.isEmpty(); }
    @Override public boolean last() { cursor = rows.size()-1; return !rows.isEmpty(); }
    @Override public int getRow() { return cursor + 1; }
    @Override public boolean absolute(int r) { cursor = r-1; return cursor >= 0 && cursor < rows.size(); }
    @Override public boolean relative(int r) { cursor += r; return cursor >= 0 && cursor < rows.size(); }
    @Override public boolean previous() { cursor--; return cursor >= 0; }
    @Override public void setFetchDirection(int d) {} @Override public int getFetchDirection() { return FETCH_FORWARD; }
    @Override public void setFetchSize(int r) {} @Override public int getFetchSize() { return rows.size(); }
    @Override public int getType() { return TYPE_FORWARD_ONLY; }
    @Override public int getConcurrency() { return CONCUR_READ_ONLY; }
    @Override public boolean rowUpdated() { return false; } @Override public boolean rowInserted() { return false; } @Override public boolean rowDeleted() { return false; }
    @Override public void updateNull(int c) {} @Override public void updateNull(String c) {}
    @Override public void updateBoolean(int c, boolean v) {} @Override public void updateBoolean(String c, boolean v) {}
    @Override public void updateByte(int c, byte v) {} @Override public void updateByte(String c, byte v) {}
    @Override public void updateShort(int c, short v) {} @Override public void updateShort(String c, short v) {}
    @Override public void updateInt(int c, int v) {} @Override public void updateInt(String c, int v) {}
    @Override public void updateLong(int c, long v) {} @Override public void updateLong(String c, long v) {}
    @Override public void updateFloat(int c, float v) {} @Override public void updateFloat(String c, float v) {}
    @Override public void updateDouble(int c, double v) {} @Override public void updateDouble(String c, double v) {}
    @Override public void updateBigDecimal(int c, BigDecimal v) {} @Override public void updateBigDecimal(String c, BigDecimal v) {}
    @Override public void updateString(int c, String v) {} @Override public void updateString(String c, String v) {}
    @Override public void updateBytes(int c, byte[] v) {} @Override public void updateBytes(String c, byte[] v) {}
    @Override public void updateDate(int c, java.sql.Date v) {} @Override public void updateDate(String c, java.sql.Date v) {}
    @Override public void updateTime(int c, java.sql.Time v) {} @Override public void updateTime(String c, java.sql.Time v) {}
    @Override public void updateTimestamp(int c, java.sql.Timestamp v) {} @Override public void updateTimestamp(String c, java.sql.Timestamp v) {}
    @Override public void updateAsciiStream(int c, InputStream v, int l) {} @Override public void updateAsciiStream(String c, InputStream v, int l) {}
    @Override public void updateBinaryStream(int c, InputStream v, int l) {} @Override public void updateBinaryStream(String c, InputStream v, int l) {}
    @Override public void updateCharacterStream(int c, Reader v, int l) {} @Override public void updateCharacterStream(String c, Reader v, int l) {}
    @Override public void updateObject(int c, Object v, int s) {} @Override public void updateObject(int c, Object v) {} @Override public void updateObject(String c, Object v, int s) {} @Override public void updateObject(String c, Object v) {}
    @Override public void insertRow() {} @Override public void updateRow() {} @Override public void deleteRow() {} @Override public void refreshRow() {}
    @Override public void cancelRowUpdates() {} @Override public void moveToInsertRow() {} @Override public void moveToCurrentRow() {}
    @Override public Statement getStatement() { return null; }
    @Override public Object getObject(int c, Map<String,Class<?>> m) { return get(c); } @Override public Object getObject(String c, Map<String,Class<?>> m) { return get(c); }
    @Override public Ref getRef(int c) { return null; } @Override public Ref getRef(String c) { return null; }
    @Override public Blob getBlob(int c) { return null; } @Override public Blob getBlob(String c) { return null; }
    @Override public Clob getClob(int c) { return null; } @Override public Clob getClob(String c) { return null; }
    @Override public Array getArray(int c) { return null; } @Override public Array getArray(String c) { return null; }
    @Override public java.sql.Date getDate(int c, Calendar cal) { return null; } @Override public java.sql.Date getDate(String c, Calendar cal) { return null; }
    @Override public java.sql.Time getTime(int c, Calendar cal) { return null; } @Override public java.sql.Time getTime(String c, Calendar cal) { return null; }
    @Override public java.sql.Timestamp getTimestamp(int c, Calendar cal) { return null; } @Override public java.sql.Timestamp getTimestamp(String c, Calendar cal) { return null; }
    @Override public java.net.URL getURL(int c) { return null; } @Override public java.net.URL getURL(String c) { return null; }
    @Override public void updateRef(int c, Ref v) {} @Override public void updateRef(String c, Ref v) {}
    @Override public void updateBlob(int c, Blob v) {} @Override public void updateBlob(String c, Blob v) {}
    @Override public void updateClob(int c, Clob v) {} @Override public void updateClob(String c, Clob v) {}
    @Override public void updateArray(int c, Array v) {} @Override public void updateArray(String c, Array v) {}
    @Override public java.sql.RowId getRowId(int c) { return null; } @Override public java.sql.RowId getRowId(String c) { return null; }
    @Override public void updateRowId(int c, java.sql.RowId v) {} @Override public void updateRowId(String c, java.sql.RowId v) {}
    @Override public int getHoldability() { return HOLD_CURSORS_OVER_COMMIT; }
    @Override public boolean isClosed() { return false; }
    @Override public void updateNString(int c, String s) {} @Override public void updateNString(String c, String s) {}
    @Override public void updateNClob(int c, java.sql.NClob n) {} @Override public void updateNClob(String c, java.sql.NClob n) {}
    @Override public java.sql.NClob getNClob(int c) { return null; } @Override public java.sql.NClob getNClob(String c) { return null; }
    @Override public java.sql.SQLXML getSQLXML(int c) { return null; } @Override public java.sql.SQLXML getSQLXML(String c) { return null; }
    @Override public void updateSQLXML(int c, java.sql.SQLXML v) {} @Override public void updateSQLXML(String c, java.sql.SQLXML v) {}
    @Override public String getNString(int c) { return get(c); } @Override public String getNString(String c) { return get(c); }
    @Override public Reader getNCharacterStream(int c) { return null; } @Override public Reader getNCharacterStream(String c) { return null; }
    @Override public void updateNCharacterStream(int c, Reader v, long l) {} @Override public void updateNCharacterStream(String c, Reader v, long l) {}
    @Override public void updateAsciiStream(int c, InputStream v, long l) {} @Override public void updateAsciiStream(String c, InputStream v, long l) {}
    @Override public void updateBinaryStream(int c, InputStream v, long l) {} @Override public void updateBinaryStream(String c, InputStream v, long l) {}
    @Override public void updateCharacterStream(int c, Reader v, long l) {} @Override public void updateCharacterStream(String c, Reader v, long l) {}
    @Override public void updateBlob(int c, InputStream v, long l) {} @Override public void updateBlob(String c, InputStream v, long l) {}
    @Override public void updateClob(int c, Reader v, long l) {} @Override public void updateClob(String c, Reader v, long l) {}
    @Override public void updateNClob(int c, Reader v, long l) {} @Override public void updateNClob(String c, Reader v, long l) {}
    @Override public <T> T getObject(int c, Class<T> type) { return null; } @Override public <T> T getObject(String c, Class<T> type) { return null; }
    @Override public <T> T unwrap(Class<T> i) throws SQLException { throw new SQLException("not a wrapper"); }
    @Override public boolean isWrapperFor(Class<?> i) { return false; }

    // Simple metadata
    private static class SimpleRSMD implements ResultSetMetaData {
        private final String[] cols;
        SimpleRSMD(String[] cols) { this.cols = cols; }
        @Override public int getColumnCount() { return cols.length; }
        @Override public String getColumnName(int i) { return cols[i-1]; }
        @Override public String getColumnLabel(int i) { return cols[i-1]; }
        @Override public int getColumnType(int i) { return java.sql.Types.VARCHAR; }
        @Override public String getColumnTypeName(int i) { return "TEXT"; }
        @Override public String getColumnClassName(int i) { return "java.lang.String"; }
        @Override public boolean isAutoIncrement(int i) { return false; }
        @Override public boolean isCaseSensitive(int i) { return true; }
        @Override public boolean isSearchable(int i) { return true; }
        @Override public boolean isCurrency(int i) { return false; }
        @Override public int isNullable(int i) { return columnNullable; }
        @Override public boolean isSigned(int i) { return false; }
        @Override public int getColumnDisplaySize(int i) { return 255; }
        @Override public int getPrecision(int i) { return 255; }
        @Override public int getScale(int i) { return 0; }
        @Override public String getTableName(int i) { return ""; }
        @Override public String getCatalogName(int i) { return ""; }
        @Override public String getSchemaName(int i) { return "public"; }
        @Override public boolean isReadOnly(int i) { return true; }
        @Override public boolean isWritable(int i) { return false; }
        @Override public boolean isDefinitelyWritable(int i) { return false; }
        @Override public <T> T unwrap(Class<T> i) throws SQLException { throw new SQLException(); }
        @Override public boolean isWrapperFor(Class<?> i) { return false; }
    }
}
