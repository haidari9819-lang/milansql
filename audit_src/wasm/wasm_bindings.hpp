#pragma once
/**
 * MilanSQL WASM Bindings
 * C API exposed to JavaScript via Emscripten embind
 *
 * Build with:
 *   emcmake cmake -B build_wasm -DCMAKE_BUILD_TYPE=Release -DMILANSQL_WASM=ON
 *   emmake ninja -C build_wasm
 */

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/bind.h>
#endif

#include <string>
#include <cstring>
#include <cstdlib>
#include "../engine/engine.hpp"
#include "../parser/parser.hpp"
#include "../dispatch.hpp"

namespace milansql {

// Global engine instance for WASM (single-threaded, in-memory)
inline Engine& wasmEngine() {
    static Engine eng;
    return eng;
}

} // namespace milansql

// C API — exported to JavaScript
extern "C" {

/**
 * Initialize MilanSQL engine.
 * Must be called before any other function.
 */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void milan_init() {
    // Engine is initialized lazily via wasmEngine()
    // Pre-create some demo data
}

/**
 * Execute a SQL statement (INSERT, CREATE, UPDATE, DELETE, etc.)
 * Returns: "OK" on success, "ERROR: message" on failure
 * Caller must free() the returned string.
 */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
char* milan_exec(const char* sql) {
    std::string result;
    std::ostringstream oss;
    std::streambuf* old = std::cout.rdbuf(oss.rdbuf());
    try {
        milansql::Parser p;
        auto cmd = p.parse(std::string(sql));
        milansql::dispatch(milansql::wasmEngine(), cmd, "wasm_sandbox");
        std::cout.rdbuf(old);
        result = oss.str();
        if (result.empty()) result = "OK";
    } catch (const std::exception& e) {
        std::cout.rdbuf(old);
        result = "ERROR: " + std::string(e.what());
    }
    char* ret = (char*)malloc(result.size() + 1);
    if (!ret) return nullptr;
    memcpy(ret, result.c_str(), result.size() + 1);
    return ret;
}

/**
 * Execute a SELECT query and return tab-separated results.
 * Format: "col1\tcol2\n" (header) + "val1\tval2\n" (rows)
 * Returns "ERROR: message" on failure.
 * Caller must free() the returned string.
 */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
char* milan_query(const char* sql) {
    return milan_exec(sql); // Output captured same way
}

/**
 * Get list of tables as newline-separated names.
 * Caller must free() the returned string.
 */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
char* milan_get_tables() {
    std::string result;
    auto& eng = milansql::wasmEngine();
    for (auto& [name, tbl] : eng.getTables()) {
        result += name + "\n";
    }
    char* ret = (char*)malloc(result.size() + 1);
    if (!ret) return nullptr;
    memcpy(ret, result.c_str(), result.size() + 1);
    return ret;
}

/**
 * Get schema for a table as "col:type\n" lines.
 */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
char* milan_get_schema(const char* tableName) {
    std::string result;
    try {
        auto& eng = milansql::wasmEngine();
        auto tbl = eng.selectAll(std::string(tableName));
        for (auto& col : tbl.columns) {
            result += col.name + ":" + col.type + "\n";
        }
    } catch (...) {
        result = "ERROR: table not found";
    }
    char* ret = (char*)malloc(result.size() + 1);
    if (!ret) return nullptr;
    memcpy(ret, result.c_str(), result.size() + 1);
    return ret;
}

/**
 * Free a string returned by any milan_* function.
 */
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void milan_free(char* ptr) {
    free(ptr);
}

} // extern "C"
