package io.milansql;
import java.sql.*;

public class MilanSQLDatabaseMetaData implements DatabaseMetaData {
    private final MilanSQLConnection conn;
    private final String host; private final int port; private final String db;

    public MilanSQLDatabaseMetaData(MilanSQLConnection conn, String host, int port, String db) {
        this.conn = conn; this.host = host; this.port = port; this.db = db;
    }

    @Override public String getDatabaseProductName() { return "MilanSQL"; }
    @Override public String getDatabaseProductVersion() { return "4.2.0"; }
    @Override public String getDriverName() { return "MilanSQL JDBC Driver"; }
    @Override public String getDriverVersion() { return "4.2.0"; }
    @Override public int getDriverMajorVersion() { return 4; }
    @Override public int getDriverMinorVersion() { return 2; }
    @Override public String getURL() { return "jdbc:milansql://" + host + ":" + port + "/" + db; }
    @Override public String getUserName() { return "root"; }
    @Override public Connection getConnection() { return conn; }
    @Override public boolean isReadOnly() { return false; }
    @Override public String getSQLKeywords() { return ""; }
    @Override public String getNumericFunctions() { return "ABS,ROUND,FLOOR,CEIL,SQRT,POWER"; }
    @Override public String getStringFunctions() { return "UPPER,LOWER,CONCAT,SUBSTR,LENGTH,TRIM"; }
    @Override public String getSystemFunctions() { return "NOW,CURRENT_TIMESTAMP"; }
    @Override public String getTimeDateFunctions() { return "NOW,YEAR,MONTH,DAY,DATE_ADD,DATEDIFF"; }
    @Override public String getSearchStringEscape() { return "\\"; }
    @Override public String getExtraNameCharacters() { return "_"; }
    @Override public int getDefaultTransactionIsolation() { return Connection.TRANSACTION_READ_COMMITTED; }
    @Override public boolean supportsBatchUpdates() { return false; }
    @Override public boolean supportsTransactions() { return true; }
    @Override public boolean supportsStoredProcedures() { return true; }
    @Override public boolean supportsOuterJoins() { return true; }
    @Override public boolean supportsFullOuterJoins() { return true; }
    @Override public boolean supportsGroupBy() { return true; }
    @Override public boolean supportsOrderByUnrelated() { return true; }
    @Override public boolean supportsUnion() { return false; }
    @Override public boolean supportsUnionAll() { return false; }
    @Override public boolean supportsSubqueriesInExists() { return true; }
    @Override public boolean supportsSubqueriesInIns() { return true; }
    @Override public boolean supportsSubqueriesInComparisons() { return true; }
    @Override public boolean supportsCorrelatedSubqueries() { return true; }

    // Return empty result sets for metadata queries
    private ResultSet emptyRS() { return MilanSQLResultSet.empty(); }
    @Override public ResultSet getTables(String c, String s, String t, String[] types) {
        try { return MilanSQLResultSet.parse(conn.sendQuery("SELECT tablename AS TABLE_NAME, 'public' AS TABLE_SCHEM, 'TABLE' AS TABLE_TYPE FROM pg_catalog.pg_tables")); }
        catch (SQLException e) { return emptyRS(); }
    }
    @Override public ResultSet getColumns(String c, String s, String t, String col) { return emptyRS(); }
    @Override public ResultSet getPrimaryKeys(String c, String s, String t) { return emptyRS(); }
    @Override public ResultSet getImportedKeys(String c, String s, String t) { return emptyRS(); }
    @Override public ResultSet getExportedKeys(String c, String s, String t) { return emptyRS(); }
    @Override public ResultSet getCrossReference(String pc,String ps,String pt,String fc,String fs,String ft) { return emptyRS(); }
    @Override public ResultSet getIndexInfo(String c,String s,String t,boolean u,boolean a) { return emptyRS(); }
    @Override public ResultSet getTypeInfo() { return emptyRS(); }
    @Override public ResultSet getCatalogs() { return emptyRS(); }
    @Override public ResultSet getSchemas() { return emptyRS(); }
    @Override public ResultSet getSchemas(String c, String s) { return emptyRS(); }
    @Override public ResultSet getProcedures(String c, String s, String p) { return emptyRS(); }
    @Override public ResultSet getProcedureColumns(String c,String s,String p,String col) { return emptyRS(); }
    @Override public ResultSet getBestRowIdentifier(String c,String s,String t,int scope,boolean nullable) { return emptyRS(); }
    @Override public ResultSet getVersionColumns(String c,String s,String t) { return emptyRS(); }
    @Override public ResultSet getTablePrivileges(String c,String s,String t) { return emptyRS(); }
    @Override public ResultSet getColumnPrivileges(String c,String s,String t,String col) { return emptyRS(); }
    @Override public ResultSet getUDTs(String c,String s,String t,int[] types) { return emptyRS(); }
    @Override public ResultSet getSuperTypes(String c,String s,String t) { return emptyRS(); }
    @Override public ResultSet getSuperTables(String c,String s,String t) { return emptyRS(); }
    @Override public ResultSet getAttributes(String c,String s,String t,String a) { return emptyRS(); }
    @Override public ResultSet getClientInfoProperties() { return emptyRS(); }
    @Override public ResultSet getFunctions(String c,String s,String f) { return emptyRS(); }
    @Override public ResultSet getFunctionColumns(String c,String s,String f,String col) { return emptyRS(); }
    @Override public ResultSet getPseudoColumns(String c,String s,String t,String col) { return emptyRS(); }

    // Boolean capability stubs
    @Override public boolean allProceduresAreCallable() { return true; }
    @Override public boolean allTablesAreSelectable() { return true; }
    @Override public boolean nullsAreSortedHigh() { return false; }
    @Override public boolean nullsAreSortedLow() { return true; }
    @Override public boolean nullsAreSortedAtStart() { return false; }
    @Override public boolean nullsAreSortedAtEnd() { return false; }
    @Override public boolean usesLocalFiles() { return true; }
    @Override public boolean usesLocalFilePerTable() { return false; }
    @Override public boolean supportsMixedCaseIdentifiers() { return true; }
    @Override public boolean storesUpperCaseIdentifiers() { return false; }
    @Override public boolean storesLowerCaseIdentifiers() { return false; }
    @Override public boolean storesMixedCaseIdentifiers() { return true; }
    @Override public boolean supportsMixedCaseQuotedIdentifiers() { return true; }
    @Override public boolean storesUpperCaseQuotedIdentifiers() { return false; }
    @Override public boolean storesLowerCaseQuotedIdentifiers() { return false; }
    @Override public boolean storesMixedCaseQuotedIdentifiers() { return true; }
    @Override public String getIdentifierQuoteString() { return "\""; }
    @Override public boolean supportsAlterTableWithAddColumn() { return true; }
    @Override public boolean supportsAlterTableWithDropColumn() { return true; }
    @Override public boolean supportsColumnAliasing() { return true; }
    @Override public boolean nullPlusNonNullIsNull() { return true; }
    @Override public boolean supportsConvert() { return false; }
    @Override public boolean supportsConvert(int from, int to) { return false; }
    @Override public boolean supportsTableCorrelationNames() { return true; }
    @Override public boolean supportsDifferentTableCorrelationNames() { return true; }
    @Override public boolean supportsExpressionsInOrderBy() { return true; }
    @Override public boolean supportsGroupByUnrelated() { return true; }
    @Override public boolean supportsGroupByBeyondSelect() { return true; }
    @Override public boolean supportsLikeEscapeClause() { return true; }
    @Override public boolean supportsMultipleResultSets() { return false; }
    @Override public boolean supportsMultipleTransactions() { return true; }
    @Override public boolean supportsNonNullableColumns() { return true; }
    @Override public boolean supportsMinimumSQLGrammar() { return true; }
    @Override public boolean supportsCoreSQLGrammar() { return true; }
    @Override public boolean supportsExtendedSQLGrammar() { return false; }
    @Override public boolean supportsANSI92EntryLevelSQL() { return true; }
    @Override public boolean supportsANSI92IntermediateSQL() { return false; }
    @Override public boolean supportsANSI92FullSQL() { return false; }
    @Override public boolean supportsIntegrityEnhancementFacility() { return false; }
    @Override public boolean supportsLimitedOuterJoins() { return true; }
    @Override public String getSchemaTerm() { return "schema"; }
    @Override public String getProcedureTerm() { return "procedure"; }
    @Override public String getCatalogTerm() { return "database"; }
    @Override public boolean isCatalogAtStart() { return true; }
    @Override public String getCatalogSeparator() { return "."; }
    @Override public boolean supportsSchemasInDataManipulation() { return true; }
    @Override public boolean supportsSchemasInProcedureCalls() { return true; }
    @Override public boolean supportsSchemasInTableDefinitions() { return true; }
    @Override public boolean supportsSchemasInIndexDefinitions() { return true; }
    @Override public boolean supportsSchemasInPrivilegeDefinitions() { return true; }
    @Override public boolean supportsCatalogsInDataManipulation() { return false; }
    @Override public boolean supportsCatalogsInProcedureCalls() { return false; }
    @Override public boolean supportsCatalogsInTableDefinitions() { return false; }
    @Override public boolean supportsCatalogsInIndexDefinitions() { return false; }
    @Override public boolean supportsCatalogsInPrivilegeDefinitions() { return false; }
    @Override public boolean supportsPositionedDelete() { return false; }
    @Override public boolean supportsPositionedUpdate() { return false; }
    @Override public boolean supportsSelectForUpdate() { return true; }
    @Override public int getMaxBinaryLiteralLength() { return 0; }
    @Override public int getMaxCharLiteralLength() { return 0; }
    @Override public int getMaxColumnNameLength() { return 0; }
    @Override public int getMaxColumnsInGroupBy() { return 0; }
    @Override public int getMaxColumnsInIndex() { return 0; }
    @Override public int getMaxColumnsInOrderBy() { return 0; }
    @Override public int getMaxColumnsInSelect() { return 0; }
    @Override public int getMaxColumnsInTable() { return 0; }
    @Override public int getMaxConnections() { return 0; }
    @Override public int getMaxCursorNameLength() { return 0; }
    @Override public int getMaxIndexLength() { return 0; }
    @Override public int getMaxSchemaNameLength() { return 0; }
    @Override public int getMaxProcedureNameLength() { return 0; }
    @Override public int getMaxCatalogNameLength() { return 0; }
    @Override public int getMaxRowSize() { return 0; }
    @Override public boolean doesMaxRowSizeIncludeBlobs() { return false; }
    @Override public int getMaxStatementLength() { return 0; }
    @Override public int getMaxStatements() { return 0; }
    @Override public int getMaxTableNameLength() { return 0; }
    @Override public int getMaxTablesInSelect() { return 0; }
    @Override public int getMaxUserNameLength() { return 0; }
    @Override public boolean supportsTransactionIsolationLevel(int l) { return l == Connection.TRANSACTION_READ_COMMITTED; }
    @Override public boolean supportsDataDefinitionAndDataManipulationTransactions() { return true; }
    @Override public boolean supportsDataManipulationTransactionsOnly() { return false; }
    @Override public boolean dataDefinitionCausesTransactionCommit() { return false; }
    @Override public boolean dataDefinitionIgnoredInTransactions() { return false; }
    @Override public boolean supportsSavepoints() { return true; }
    @Override public boolean supportsNamedParameters() { return false; }
    @Override public boolean supportsMultipleOpenResults() { return false; }
    @Override public boolean supportsGetGeneratedKeys() { return false; }
    @Override public int getResultSetHoldability() { return ResultSet.HOLD_CURSORS_OVER_COMMIT; }
    @Override public int getDatabaseMajorVersion() { return 4; }
    @Override public int getDatabaseMinorVersion() { return 2; }
    @Override public int getJDBCMajorVersion() { return 4; }
    @Override public int getJDBCMinorVersion() { return 2; }
    @Override public int getSQLStateType() { return sqlStateSQL; }
    @Override public boolean locatorsUpdateCopy() { return false; }
    @Override public boolean supportsStatementPooling() { return false; }
    @Override public java.sql.RowIdLifetime getRowIdLifetime() { return java.sql.RowIdLifetime.ROWID_UNSUPPORTED; }
    @Override public boolean supportsStoredFunctionsUsingCallSyntax() { return false; }
    @Override public boolean autoCommitFailureClosesAllResultSets() { return false; }
    @Override public boolean generatedKeyAlwaysReturned() { return false; }
    @Override public <T> T unwrap(Class<T> i) throws SQLException { throw new SQLException(); }
    @Override public boolean isWrapperFor(Class<?> i) { return false; }
}
