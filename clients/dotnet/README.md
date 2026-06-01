# MilanSQL.Data — .NET ADO.NET Driver

ADO.NET / DbProviderFactory-compatible driver for MilanSQL.

## NuGet

```
dotnet add package MilanSQL.Data --version 4.2.0
```

## Usage

```csharp
using MilanSQL.Data;

var conn = new MilanSQLConnection("Server=localhost;Port=4406;Database=public;User=root");
conn.Open();

var cmd = conn.CreateCommand();
cmd.CommandText = "SELECT * FROM users WHERE id = @id";
cmd.Parameters.AddWithValue("@id", 1);

using var reader = cmd.ExecuteReader();
while (reader.Read())
    Console.WriteLine($"{reader["id"]}: {reader["name"]}");

conn.Close();
```

## Build

```bash
cd clients/dotnet/MilanSQL.Data
dotnet build
```
