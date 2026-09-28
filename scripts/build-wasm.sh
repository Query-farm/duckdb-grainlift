#!/usr/bin/env bash
# Build grainlift.duckdb_extension.wasm for haybarn-wasm.
#
#   scripts/build-wasm.sh [wasm_threads|wasm_eh]     (default: wasm_threads, the COI build)
#
# Env:
#   EMSDK_DIR            emsdk checkout with 5.0.7 activated (default ~/Development/emsdk)
#   GRAINLIFT_SOURCE_DIR grainlift Cargo workspace (default ./grainlift)
#   DEPLOY_DIR           optional: copy the .wasm into this local extension repository
#                        directory (e.g. <repo>/<engine-rev>/wasm_threads)
#
# No vcpkg is needed: the extension's only native dependencies (openssl/curl
# for httpfs) are excluded on emscripten, and the Rust driver is built by cargo
# (nightly + -Z build-std, see cmake/grainlift_rust.cmake).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
PLATFORM="${1:-wasm_threads}"
EMSDK_DIR="${EMSDK_DIR:-$HOME/Development/emsdk}"
BUILD_DIR="$SCRIPT_DIR/build/$PLATFORM"

EMSDK_QUIET=1 source "$EMSDK_DIR/emsdk_env.sh" >/dev/null
emcc --version | head -1

COMMON_FLAGS="-fwasm-exceptions -DWEBDB_FAST_EXCEPTIONS=1"
case "$PLATFORM" in
  wasm_threads)
    CXX_FLAGS="$COMMON_FLAGS -msimd128 -DWEBDB_SIMD=1 -mbulk-memory -DWEBDB_BULK_MEMORY=1 -pthread -sUSE_PTHREADS=1 -sSHARED_MEMORY=1 -DWEBDB_THREADS=1"
    THREAD_ARGS=(-DUSE_WASM_THREADS=TRUE)
    ;;
  wasm_eh)
    CXX_FLAGS="$COMMON_FLAGS"
    THREAD_ARGS=()
    ;;
  *)
    echo "unsupported platform: $PLATFORM (wasm_threads or wasm_eh)" >&2
    exit 1
    ;;
esac

emcmake cmake -G Ninja \
  -S "$SCRIPT_DIR/duckdb" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DDUCKDB_EXTENSION_CONFIGS="$SCRIPT_DIR/extension_config.cmake" \
  -DWASM_LOADABLE_EXTENSIONS=1 \
  -DBUILD_EXTENSIONS_ONLY=1 \
  -DEXTENSION_STATIC_BUILD=1 \
  -DSKIP_EXTENSIONS="parquet;jemalloc" \
  "${THREAD_ARGS[@]}" \
  -DCMAKE_C_FLAGS="$CXX_FLAGS" \
  -DCMAKE_CXX_FLAGS="$CXX_FLAGS" \
  -DDUCKDB_EXPLICIT_PLATFORM="$PLATFORM" \
  -DDUCKDB_CUSTOM_PLATFORM="$PLATFORM"

cmake --build "$BUILD_DIR" --target grainlift_loadable_extension

WASM="$(find "$BUILD_DIR/extension/grainlift" -name 'grainlift.duckdb_extension.wasm' | head -1)"
echo "built: $WASM ($(du -h "$WASM" | cut -f1))"

# Native wasm exception handling end to end: legacy emscripten EH imports mean
# some object (usually a Rust std built without -Z build-std) will throw
# "TypeError: c is not a function" on the eh engine.
LEGACY_EH=$(wasm-objdump -x "$WASM" | grep -cE '<env\.(invoke_|__resumeException|getTempRet0)' || true)
if [ "$LEGACY_EH" != "0" ]; then
  echo "ERROR: $LEGACY_EH legacy exception-handling imports in $WASM" >&2
  exit 1
fi

if [ -n "${DEPLOY_DIR:-}" ]; then
  mkdir -p "$DEPLOY_DIR"
  cp "$WASM" "$DEPLOY_DIR/"
  echo "deployed to $DEPLOY_DIR"
fi
