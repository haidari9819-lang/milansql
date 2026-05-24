cmake_minimum_required(VERSION 3.16)

# Projektname und Version
project(milansql VERSION 0.1.0 LANGUAGES CXX)

# C++17 Standard festlegen
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Alle Quelldateien sammeln
set(SOURCES
    src/main.cpp
)

# Executable definieren
add_executable(milansql ${SOURCES})

# Include-Verzeichnisse hinzufügen
target_include_directories(milansql PRIVATE src)

# Compiler-Warnungen aktivieren
if(MSVC)
    target_compile_options(milansql PRIVATE /W4)
else()
    target_compile_options(milansql PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Ausgabe-Info
message(STATUS "MilanSQL v${PROJECT_VERSION} — Build konfiguriert")
message(STATUS "C++ Standard: C++${CMAKE_CXX_STANDARD}")
