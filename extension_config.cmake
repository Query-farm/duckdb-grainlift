# This file is included by DuckDB's build system. It specifies which extension to load

# For WASM the side module is linked by a post-build `emcc` step that only sees
# LINKED_LIBS, so the Rust driver archive is passed here. Its path mirrors
# cmake/grainlift_rust.cmake (cargo target dir inside the DuckDB build tree).
if(EMSCRIPTEN)
    set(GRAINLIFT_WASM_RUST_LIB "${CMAKE_BINARY_DIR}/cargo/wasm32-unknown-emscripten/release/libadbc_driver_grainlift.a")
endif()

duckdb_extension_load(grainlift
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
    EXTENSION_VERSION v0.6.1
    LINKED_LIBS "${GRAINLIFT_WASM_RUST_LIB}"
)

# Native builds use httpfs as DuckDB's HTTPUtil implementation (DuckDB-WASM
# provides its own XHR-based HTTPUtil in the engine).
option(GRAINLIFT_BUILD_HTTPFS "Build httpfs alongside the native test engine" OFF)
if(NOT EMSCRIPTEN AND GRAINLIFT_BUILD_HTTPFS)
    duckdb_extension_load(httpfs
        GIT_URL https://github.com/Query-farm-haybarn/haybarn-httpfs
        GIT_TAG 9c5025d
    )
endif()
