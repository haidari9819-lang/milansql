cmake_minimum_required(VERSION 3.16)

# Projektname und Version
project(milansql VERSION 9.3.0 LANGUAGES CXX)

# C++17 Standard festlegen
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Phase 110: Optional OpenSSL for TLS (Linux)
find_package(OpenSSL QUIET OPTIONAL_COMPONENTS SSL Crypto)

# Alle Quelldateien sammeln
set(SOURCES
    src/main.cpp
)

# ── Haupt-Executable ──────────────────────────────────────────
add_executable(milansql ${SOURCES})
target_include_directories(milansql PRIVATE src src/server src/auth src/crypto src/optimizer src/cache src/utils src/backup src/replication src/scheduler src/locking src/pubsub src/parallel src/fdw src/extensions src/extensions/builtin src/copy src/pool src/compression src/timeseries src/cdc src/api src/monitoring src/federation src/config src/migration src/ssl src/index src/concurrent)

# Compiler-Warnungen aktivieren
if(MSVC)
    target_compile_options(milansql PRIVATE /W4)
else()
    target_compile_options(milansql PRIVATE -Wall -Wextra -Wpedantic -Wmissing-field-initializers)
endif()

# Phase 47: Link ws2_32 on Windows (both MSVC and MinGW/MSYS2 use Winsock2)
# Phase 110: Also link Secur32 and Crypt32 for SChannel TLS
if(WIN32)
    target_link_libraries(milansql PRIVATE ws2_32 secur32 crypt32)
endif()

# Phase 110: Link OpenSSL if available (Linux/non-Windows)
if(OpenSSL_FOUND AND NOT WIN32)
    target_link_libraries(milansql PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(milansql PRIVATE HAVE_OPENSSL=1)
    message(STATUS "OpenSSL found — TLS enabled for milansql")
else()
    message(STATUS "OpenSSL not found or Windows (SChannel used) — milansql will use platform TLS")
endif()

# ── Test-Suite (Phase 99) ────────────────────────────────────
add_executable(milansql_tests src/tests/milansql_tests.cpp)
target_compile_features(milansql_tests PRIVATE cxx_std_17)
target_include_directories(milansql_tests PRIVATE src src/server src/auth src/crypto src/locking src/optimizer src/cache src/utils src/backup src/replication src/scheduler src/pubsub src/parallel src/compression src/copy src/pool src/fdw src/cdc src/api src/extensions src/extensions/builtin src/timeseries src/types src/mvcc src/wal src/buffer src/spatial src/profiler src/storage src/federation src/config src/migration src/ssl src/index src/concurrent src/debug src/executor)
if(MSVC)
    target_compile_options(milansql_tests PRIVATE /W4)
else()
    target_compile_options(milansql_tests PRIVATE -Wall -Wextra -Wmissing-field-initializers)
endif()
if(WIN32)
    target_link_libraries(milansql_tests PRIVATE ws2_32 secur32 crypt32)
endif()

# ── Stress Test (Phase 99) ───────────────────────────────────
add_executable(milansql_stress src/tests/stress_test.cpp)
target_compile_features(milansql_stress PRIVATE cxx_std_17)
target_include_directories(milansql_stress PRIVATE src src/server src/locking src/optimizer src/cache src/utils src/backup src/replication src/scheduler src/pubsub src/parallel src/compression src/copy src/pool src/fdw src/cdc src/api src/extensions src/extensions/builtin src/timeseries src/types src/mvcc src/wal src/buffer src/spatial src/profiler src/storage src/debug)
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

# ── Phase 152: Fuzz Hardening ────────────────────────────────
add_executable(milansql_fuzz_hardening src/tests/fuzz_hardening.cpp)
target_compile_features(milansql_fuzz_hardening PRIVATE cxx_std_17)
target_include_directories(milansql_fuzz_hardening PRIVATE src src/server src/locking src/optimizer src/cache src/utils src/backup src/replication src/scheduler src/pubsub src/parallel src/compression src/copy src/pool src/fdw src/cdc src/api src/extensions src/extensions/builtin src/timeseries src/types src/mvcc src/wal src/buffer src/spatial src/profiler src/storage src/federation src/config src/migration src/ssl src/index src/concurrent src/debug src/executor)
if(MSVC)
    target_compile_options(milansql_fuzz_hardening PRIVATE /W4)
else()
    target_compile_options(milansql_fuzz_hardening PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
if(WIN32)
    target_link_libraries(milansql_fuzz_hardening PRIVATE ws2_32)
endif()

# ── Phase 117: Embedded Mode ─────────────────────────────────
# Enable C language for the embedded C API test
enable_language(C)
# Static library: milansql_embedded
add_library(milansql_embedded STATIC src/embedded/embedded_api.cpp)
target_compile_features(milansql_embedded PRIVATE cxx_std_17)
target_include_directories(milansql_embedded PUBLIC include)
target_include_directories(milansql_embedded PRIVATE
    src src/server src/auth src/crypto src/locking src/optimizer src/cache src/utils
    src/backup src/replication src/scheduler src/pubsub src/parallel
    src/compression src/copy src/pool src/fdw src/cdc src/api
    src/extensions src/extensions/builtin src/timeseries src/types
    src/mvcc src/wal src/buffer src/spatial src/profiler src/storage
    src/federation src/config src/migration src/ssl src/index
    src/concurrent src/document src/graph)
if(MSVC)
    target_compile_options(milansql_embedded PRIVATE /W4)
else()
    target_compile_options(milansql_embedded PRIVATE -Wall -Wextra -Wmissing-field-initializers)
endif()
if(WIN32)
    target_link_libraries(milansql_embedded PRIVATE ws2_32 secur32 crypt32)
endif()

# C test executable: milansql_embedded_test (compiled as C, links C++ lib)
add_executable(milansql_embedded_test src/tests/embedded_test.c)
target_include_directories(milansql_embedded_test PRIVATE include)
target_link_libraries(milansql_embedded_test PRIVATE milansql_embedded)
# C test needs the C++ runtime via the C++ linker driver
set_target_properties(milansql_embedded_test PROPERTIES LINKER_LANGUAGE CXX)
if(WIN32)
    target_link_libraries(milansql_embedded_test PRIVATE ws2_32 secur32 crypt32)
    # Force console subsystem so WinMain is not required
    target_compile_options(milansql_embedded_test PRIVATE -mconsole)
    target_link_options(milansql_embedded_test PRIVATE -mconsole)
endif()

# AddressSanitizer build option
option(ASAN "Enable AddressSanitizer" OFF)
if(ASAN)
  foreach(tgt milansql milansql_tests)
    if(TARGET ${tgt})
      target_compile_options(${tgt} PRIVATE
        -fsanitize=address
        -fsanitize=undefined
        -fno-omit-frame-pointer
        -g
      )
      target_link_options(${tgt} PRIVATE
        -fsanitize=address
        -fsanitize=undefined
      )
    endif()
  endforeach()
endif()

# ── CTest Integration ────────────────────────────────────────
enable_testing()
add_test(NAME milansql_tests          COMMAND milansql_tests)
add_test(NAME milansql_embedded_tests COMMAND milansql_embedded_test)

# Ausgabe-Info
message(STATUS "MilanSQL v${PROJECT_VERSION} — Build konfiguriert")
message(STATUS "C++ Standard: C++${CMAKE_CXX_STANDARD}")
