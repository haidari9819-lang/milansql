# Contributing to MilanSQL

Thank you for your interest in contributing to MilanSQL!

## Build Requirements

- CMake 3.16+
- C++17 compiler: GCC 10+, Clang 12+, or MSVC 2019+
- Windows: MSYS2/UCRT64 (`export PATH="/c/msys64/ucrt64/bin:$PATH"`)
- No external libraries required

## Building

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

## Running Tests

```bash
./build/milansql_tests.exe   # 223 automated tests
./build/milansql_bench.exe   # benchmarks
./build/milansql_stress.exe  # stress tests (10k INSERTs, 1k TXs)
```

All 223 tests must pass before submitting a PR.

## Code Style

- Header-only C++17: all logic in `.hpp` files in `src/`
- Zero external dependencies — stdlib only
- No exceptions across module boundaries (return error strings)
- Every new feature needs at least one test in `src/tests/milansql_tests.cpp`
- 0 compiler warnings with `-Wall -Wextra -Wpedantic`

## Adding a Feature (Phase N+1)

1. Read relevant existing files before touching anything
2. Implement in `src/feature/feature.hpp`
3. Add parser support in `src/parser/parser.hpp`
4. Add dispatch in `src/dispatch.hpp`
5. Update `CMakeLists.txt` include paths
6. Add test group in `src/tests/milansql_tests.cpp`
7. Update `README.md` feature matrix
8. Single commit: `"Phase N: Short Description"`

## Commit Convention

```
Phase NN: Short Description
docs: Update README for feature X
fix: Correct NULL handling in GROUP BY
```

## Architecture Overview

See `README.md` → Architecture section for the full file tree.
The core data flow: SQL string → `parser.hpp` → `ParsedCommand` → `dispatch.hpp` → `engine.hpp`.
