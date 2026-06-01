# Building MilanSQL for WebAssembly (WASM)

## Requirements
- [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html)
- CMake 3.16+
- Python 3 (for emsdk)

## Setup Emscripten

```bash
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk
./emsdk install latest
./emsdk activate latest
source ./emsdk_env.sh
```

## Build

```bash
cd milansql

# Configure for WASM
emcmake cmake -B build_wasm \
    -DCMAKE_BUILD_TYPE=Release \
    -DMILANSQL_WASM=ON \
    -DCMAKE_EXE_LINKER_FLAGS="-s WASM=1 -s EXPORTED_RUNTIME_METHODS=['cwrap','ccall'] -s ALLOW_MEMORY_GROWTH=1 -s NO_FILESYSTEM=0 --bind"

# Build
emmake cmake --build build_wasm --target milansql_wasm

# Output
ls build_wasm/milansql.js   # JavaScript glue
ls build_wasm/milansql.wasm # WebAssembly binary
```

## CMake WASM target

Add to `CMakeLists.txt` (when building with Emscripten):

```cmake
if(DEFINED MILANSQL_WASM)
    add_executable(milansql_wasm src/wasm/wasm_main.cpp)
    target_include_directories(milansql_wasm PRIVATE src)
    target_compile_options(milansql_wasm PRIVATE -O2)
    set_target_properties(milansql_wasm PROPERTIES
        SUFFIX ".js"
        LINK_FLAGS "-s WASM=1 -s ALLOW_MEMORY_GROWTH=1 -s EXPORTED_FUNCTIONS=['_milan_init','_milan_exec','_milan_query','_milan_free'] -s EXPORTED_RUNTIME_METHODS=['ccall','cwrap']"
    )
endif()
```

## Browser Usage

```html
<script src="milansql.js"></script>
<script>
Module.onRuntimeInitialized = function() {
    const milan_init = Module.cwrap('milan_init', null, []);
    const milan_exec = Module.cwrap('milan_exec', 'string', ['string']);

    milan_init();
    console.log(milan_exec("CREATE TABLE test (id INT, name TEXT)"));
    console.log(milan_exec("INSERT INTO test VALUES (1, 'Alice')"));
    console.log(milan_exec("SELECT * FROM test"));
};
</script>
```

## Demo Page

Open `docs/demo.html` in a browser. The demo uses a JavaScript shim
(`docs/wasm/milansql-wasm-shim.js`) that simulates the WASM API.

Replace with the real `milansql.js` from the Emscripten build for production.
