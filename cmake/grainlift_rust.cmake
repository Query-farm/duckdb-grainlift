# Build the grainlift ADBC driver as a Rust staticlib and expose it as the
# `grainlift_rust` imported library (plus the `grainlift_rust_build` target).
#
# Native: host toolchain, host target.
# WASM:   wasm32-unknown-emscripten with a pinned nightly and -Z build-std so
#         std uses native wasm exception handling (matching the engine's
#         -fwasm-exceptions); the COI (wasm_threads) build also enables
#         shared-memory atomics. This mirrors haybarn-extension-ci-tools.

set(GRAINLIFT_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../grainlift" CACHE PATH "grainlift Cargo workspace")
if(DEFINED ENV{GRAINLIFT_SOURCE_DIR})
    # Persist in the cache so build-time regeneration keeps the override.
    set(GRAINLIFT_SOURCE_DIR "$ENV{GRAINLIFT_SOURCE_DIR}" CACHE PATH "grainlift Cargo workspace" FORCE)
endif()
get_filename_component(GRAINLIFT_SOURCE_DIR "${GRAINLIFT_SOURCE_DIR}" ABSOLUTE)
if(NOT EXISTS "${GRAINLIFT_SOURCE_DIR}/Cargo.toml")
    message(FATAL_ERROR "grainlift sources not found at ${GRAINLIFT_SOURCE_DIR} (set GRAINLIFT_SOURCE_DIR)")
endif()

if(EMSCRIPTEN)
    # iroh:// through Haybarn's Iroh adapter Worker (works on the COI build;
    # the eh build reports that it needs COI).
    set(GRAINLIFT_DEFAULT_FEATURES "host-http,iroh-browser")
else()
    set(GRAINLIFT_DEFAULT_FEATURES "host-http")
endif()
# Override with -DGRAINLIFT_CARGO_FEATURES=...; the default is not cached so it
# follows the platform.
if(NOT GRAINLIFT_CARGO_FEATURES)
    set(GRAINLIFT_CARGO_FEATURES "${GRAINLIFT_DEFAULT_FEATURES}")
endif()
set(GRAINLIFT_CARGO_PROFILE "release" CACHE STRING "Cargo profile for adbc-driver-grainlift")
set(GRAINLIFT_CARGO_TARGET_DIR "${CMAKE_BINARY_DIR}/cargo")

set(GRAINLIFT_CARGO_ENV "")
# `cargo rustc --crate-type staticlib` builds only the archive; the crate's
# cdylib output cannot be linked for emscripten (and is not needed here).
set(GRAINLIFT_CARGO_ARGS
    rustc --profile ${GRAINLIFT_CARGO_PROFILE}
    --manifest-path "${GRAINLIFT_SOURCE_DIR}/Cargo.toml"
    -p adbc-driver-grainlift --lib --crate-type staticlib
    --no-default-features --features "${GRAINLIFT_CARGO_FEATURES}"
    --target-dir "${GRAINLIFT_CARGO_TARGET_DIR}")

if(EMSCRIPTEN)
    set(GRAINLIFT_RUST_TOOLCHAIN "nightly-2026-05-20" CACHE STRING "Rust toolchain for WASM builds")
    set(GRAINLIFT_RUST_TARGET "wasm32-unknown-emscripten")
    list(APPEND GRAINLIFT_CARGO_ARGS --target ${GRAINLIFT_RUST_TARGET})
    list(APPEND GRAINLIFT_CARGO_ENV "RUSTUP_TOOLCHAIN=${GRAINLIFT_RUST_TOOLCHAIN}"
         "CARGO_UNSTABLE_BUILD_STD=std,panic_abort")
    # C code built by -sys crates (zstd) ends up in a side module: it must be
    # position independent and use the engine's exception/thread ABI.
    set(GRAINLIFT_WASM_CFLAGS "-fPIC -fwasm-exceptions")
    if(USE_WASM_THREADS OR WASM_THREAD_FLAGS OR "${DUCKDB_EXPLICIT_PLATFORM}" STREQUAL "wasm_threads")
        list(APPEND GRAINLIFT_CARGO_ENV
             "CARGO_TARGET_WASM32_UNKNOWN_EMSCRIPTEN_RUSTFLAGS=-Ctarget-feature=+atomics,+bulk-memory,+mutable-globals")
        set(GRAINLIFT_WASM_CFLAGS "${GRAINLIFT_WASM_CFLAGS} -pthread -matomics -mbulk-memory")
    endif()
    list(APPEND GRAINLIFT_CARGO_ENV "CFLAGS_wasm32_unknown_emscripten=${GRAINLIFT_WASM_CFLAGS}")
    set(GRAINLIFT_RUST_OUT_DIR "${GRAINLIFT_CARGO_TARGET_DIR}/${GRAINLIFT_RUST_TARGET}/${GRAINLIFT_CARGO_PROFILE}")
else()
    set(GRAINLIFT_RUST_OUT_DIR "${GRAINLIFT_CARGO_TARGET_DIR}/${GRAINLIFT_CARGO_PROFILE}")
endif()

set(GRAINLIFT_RUST_LIB "${GRAINLIFT_RUST_OUT_DIR}/libadbc_driver_grainlift.a")
# Consumed by extension_config.cmake (LINKED_LIBS for the WASM side module).
set(GRAINLIFT_RUST_LIB "${GRAINLIFT_RUST_LIB}" CACHE INTERNAL "")

find_program(CARGO_EXECUTABLE cargo HINTS "$ENV{HOME}/.cargo/bin" REQUIRED)

# cargo tracks its own inputs; always invoke it and let it no-op when current.
add_custom_target(grainlift_rust_build
    COMMAND ${CMAKE_COMMAND} -E env ${GRAINLIFT_CARGO_ENV} ${CARGO_EXECUTABLE} ${GRAINLIFT_CARGO_ARGS}
    BYPRODUCTS "${GRAINLIFT_RUST_LIB}"
    WORKING_DIRECTORY "${GRAINLIFT_SOURCE_DIR}"
    COMMENT "Building grainlift ADBC driver (${GRAINLIFT_CARGO_FEATURES})"
    USES_TERMINAL
    VERBATIM)

add_library(grainlift_rust STATIC IMPORTED GLOBAL)
set_target_properties(grainlift_rust PROPERTIES IMPORTED_LOCATION "${GRAINLIFT_RUST_LIB}")
add_dependencies(grainlift_rust grainlift_rust_build)
if(APPLE)
    set_property(TARGET grainlift_rust APPEND PROPERTY INTERFACE_LINK_LIBRARIES
        "-framework CoreFoundation" "-framework Security" "-framework SystemConfiguration" iconv)
elseif(UNIX AND NOT EMSCRIPTEN)
    set_property(TARGET grainlift_rust APPEND PROPERTY INTERFACE_LINK_LIBRARIES pthread dl m)
endif()
