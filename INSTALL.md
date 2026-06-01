# Installing MilanSQL

## Docker (Recommended)

```bash
docker run -p 4406:4406 -p 8080:8080 milansql/milansql
```

Or with all protocols:
```bash
docker run -p 4406:4406 -p 4407:4407 -p 5433:5433 -p 8080:8080 -p 8081:8081 \
  milansql/milansql
```

## Build from Source

**Requirements:** CMake 3.16+, C++17 compiler

```bash
git clone https://github.com/haidari9819-lang/milansql
cd milansql
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/milansql --server --port 4406
```

**Windows (MSYS2 UCRT64):**
```bash
export PATH="/c/msys64/ucrt64/bin:$PATH"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

## Homebrew (macOS/Linux)

```bash
brew install milansql
```

## Client Libraries

### Python
```bash
pip install milansql
```

### Node.js
```bash
npm install milansql-client
```

### Java (Maven)
```xml
<dependency>
    <groupId>io.milansql</groupId>
    <artifactId>milansql-jdbc</artifactId>
    <version>4.2.0</version>
</dependency>
```

### .NET
```bash
dotnet add package MilanSQL.Data --version 4.2.0
```

### Rust
```toml
[dependencies]
milansql = "4.2.0"
```

## psql Connection

```bash
psql -h localhost -p 5433 -U root -d public
```

## REST API

```bash
curl http://localhost:8080/status
curl -X POST http://localhost:8080/query \
  -H "Content-Type: application/json" \
  -d '{"sql": "SELECT * FROM users"}'
```

## GraphQL

```bash
curl http://localhost:8081/graphql/playground
```
