cmake_minimum_required(VERSION 3.16)

# Projektname und Version
project(milansql VERSION 4.3.0 LANGUAGES CXX)

# C++17 Standard festlegen
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Alle Quelldateien sammeln
set(SOURCES
    src/main.cpp
)

# ── Haupt-Executable ──────────────────────────────────────────
add_executable(milansql ${SOURCES})
target_include_directories(milansql PRIVATE src src/server src/optimizer src/cache src/utils src/backup src/replication src/scheduler src/locking src/pubsub src/parallel src/fdw src/extensions src/extensions/builtin src/copy src/pool src/compression src/timeseries src/cdc src/api src/monitoring src/federation)

# Compiler-Warnungen aktivieren
if(MSVC)
    target_compile_options(milansql PRIVATE /W4)
else()
    target_compile_options(milansql PRIVATE -Wall -Wextra -Wpedantic -Wmissing-field-initializers)
endif()

# Phase 47: Link ws2_32 on Windows (both MSVC and MinGW/MSYS2 use Winsock2)
if(WIN32)
    target_link_libraries(milansql PRIVATE ws2_32)
endif()

# ── Test-Suite (Phase 99) ────────────────────────────────────
add_executable(milansql_tests src/tests/milansql_tests.cpp)
target_compile_features(milansql_tests PRIVATE cxx_std_17)
target_include_directories(milansql_tests PRIVATE src src/server src/locking src/optimizer src/cache src/utils src/backup src/replication src/scheduler src/pubsub src/parallel src/compression src/copy src/pool src/fdw src/cdc src/api src/extensions src/extensions/builtin src/timeseries src/types src/mvcc src/wal src/buffer src/spatial src/profiler src/storage src/federation)
if(MSVC)
    target_compile_options(milansql_tests PRIVATE /W4)
else()
    target_compile_options(milansql_tests PRIVATE -Wall -Wextra -Wmissing-field-initializers)
endif()
if(WIN32)
    target_link_libraries(milansql_tests PRIVATE ws2_32)
endif()

# ── Stress Test (Phase 99) ───────────────────────────────────
add_executable(milansql_stress src/tests/stress_test.cpp)
target_compile_features(milansql_stress PRIVATE cxx_std_17)
target_include_directories(milansql_stress PRIVATE src src/server src/locking src/optimizer src/cache src/utils src/backup src/replication src/scheduler src/pubsub src/parallel src/compression src/copy src/pool src/fdw src/cdc src/api src/extensions src/extensions/builtin src/timeseries src/types src/mvcc src/wal src/buffer src/spatial src/profiler src/storage)
if(MSVC)
    target_compile_options(milansql_stress PRIVATE /W4)
else()
    target_compile_options(milansql_stress PRIVATE -Wall -Wextra -Wmissing-field-initializers)
endif()
if(WIN32)
    target_link_libraries(milansql_stress PRIVATE ws2_32)
endif()

# ── Fuzzer (Phase 101) ───────────────────────────────────────
add_executable(milansql_fuzzer src/tests/sql_fuzzer.cpp)
target_compile_features(milansql_fuzzer PRIVATE cxx_std_17)
target_include_directories(milansql_fuzzer PRIVATE src src/server src/locking src/optimizer src/cache src/utils src/backup src/replication src/scheduler src/pubsub src/parallel src/compression src/copy src/pool src/fdw src/cdc src/api src/extensions src/timeseries src/types)
if(MSVC)
    target_compile_options(milansql_fuzzer PRIVATE /W4)
else()
    target_compile_options(milansql_fuzzer PRIVATE -Wall -Wextra -Wmissing-field-initializers)
endif()
if(WIN32)
    target_link_libraries(milansql_fuzzer PRIVATE ws2_32)
endif()

# ── TPC-H Benchmark (Phase 101) ──────────────────────────────
add_executable(milansql_tpch src/tests/tpch_benchmark.cpp)
target_compile_features(milansql_tpch PRIVATE cxx_std_17)
target_include_directories(milansql_tpch PRIVATE src src/server src/locking src/optimizer src/cache src/utils src/backup src/replication src/scheduler src/pubsub src/parallel src/compression src/copy src/pool src/fdw src/cdc src/api src/extensions src/timeseries src/types)
if(MSVC)
    target_compile_options(milansql_tpch PRIVATE /W4)
else()
    target_compile_options(milansql_tpch PRIVATE -Wall -Wextra -Wmissing-field-initializers)
endif()
if(WIN32)
    target_link_libraries(milansql_tpch PRIVATE ws2_32)
endif()

# ── Benchmark (Phase 39.5) ───────────────────────────────────
add_executable(milansql_bench src/tests/benchmark.cpp)
target_compile_features(milansql_bench PRIVATE cxx_std_17)
target_include_directories(milansql_bench PRIVATE src src/locking)
if(MSVC)
    target_compile_options(milansql_bench PRIVATE /W4)
else()
    target_compile_options(milansql_bench PRIVATE -Wall -Wextra)
endif()

# ── CTest Integration ────────────────────────────────────────
enable_testing()
add_test(NAME milansql_tests COMMAND milansql_tests)

# Ausgabe-Info
message(STATUS "MilanSQL v${PROJECT_VERSION} — Build konfiguriert")
message(STATUS "C++ Standard: C++${CMAKE_CXX_STANDARD}")
