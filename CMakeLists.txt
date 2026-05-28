cmake_minimum_required(VERSION 3.16)

# Projektname und Version
project(milansql VERSION 1.3.0 LANGUAGES CXX)

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
target_include_directories(milansql PRIVATE src src/server src/optimizer src/cache src/utils src/backup src/replication src/scheduler)

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

# ── Test-Suite (Phase 39.5) ──────────────────────────────────
add_executable(milansql_tests src/tests/milansql_tests.cpp)
target_compile_features(milansql_tests PRIVATE cxx_std_17)
target_include_directories(milansql_tests PRIVATE src src/server)
if(MSVC)
    target_compile_options(milansql_tests PRIVATE /W4)
else()
    target_compile_options(milansql_tests PRIVATE -Wall -Wextra -Wmissing-field-initializers)
endif()

# ── Benchmark (Phase 39.5) ───────────────────────────────────
add_executable(milansql_bench src/tests/benchmark.cpp)
target_compile_features(milansql_bench PRIVATE cxx_std_17)
target_include_directories(milansql_bench PRIVATE src)
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
