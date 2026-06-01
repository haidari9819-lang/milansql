# MilanSQL JDBC Driver

Pure Java JDBC 4.2 driver for MilanSQL database.

## Maven

```xml
<dependency>
    <groupId>io.milansql</groupId>
    <artifactId>milansql-jdbc</artifactId>
    <version>4.2.0</version>
</dependency>
```

## Usage

```java
import java.sql.*;
Class.forName("io.milansql.MilanSQLDriver");
Connection conn = DriverManager.getConnection("jdbc:milansql://localhost:4406/public");
Statement stmt = conn.createStatement();
ResultSet rs = stmt.executeQuery("SELECT * FROM users");
while (rs.next()) {
    System.out.println(rs.getInt("id") + ": " + rs.getString("name"));
}
conn.close();
```

## Build

```bash
cd clients/java
mvn package
```
