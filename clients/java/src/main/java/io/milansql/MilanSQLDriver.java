package io.milansql;

import java.sql.*;
import java.util.Properties;
import java.util.logging.Logger;

public class MilanSQLDriver implements Driver {
    public static final String URL_PREFIX = "jdbc:milansql://";
    public static final int MAJOR_VERSION = 4;
    public static final int MINOR_VERSION = 2;

    static {
        try {
            DriverManager.registerDriver(new MilanSQLDriver());
        } catch (SQLException e) {
            throw new RuntimeException("Failed to register MilanSQLDriver", e);
        }
    }

    @Override
    public Connection connect(String url, Properties info) throws SQLException {
        if (!acceptsURL(url)) return null;
        // Parse: jdbc:milansql://host:port/database
        String stripped = url.substring(URL_PREFIX.length()); // "host:port/database"
        String host = "localhost";
        int port = 4406;
        String database = "public";
        int slashIdx = stripped.indexOf('/');
        String hostport = slashIdx >= 0 ? stripped.substring(0, slashIdx) : stripped;
        if (slashIdx >= 0) database = stripped.substring(slashIdx + 1);
        int colonIdx = hostport.indexOf(':');
        if (colonIdx >= 0) {
            host = hostport.substring(0, colonIdx);
            port = Integer.parseInt(hostport.substring(colonIdx + 1));
        } else {
            host = hostport;
        }
        String user = info != null ? info.getProperty("user", "root") : "root";
        return new MilanSQLConnection(host, port, database, user);
    }

    @Override
    public boolean acceptsURL(String url) throws SQLException {
        return url != null && url.startsWith(URL_PREFIX);
    }

    @Override
    public DriverPropertyInfo[] getPropertyInfo(String url, Properties info) {
        return new DriverPropertyInfo[0];
    }

    @Override public int getMajorVersion() { return MAJOR_VERSION; }
    @Override public int getMinorVersion() { return MINOR_VERSION; }
    @Override public boolean jdbcCompliant() { return false; }
    @Override public Logger getParentLogger() { return Logger.getLogger("io.milansql"); }
}
