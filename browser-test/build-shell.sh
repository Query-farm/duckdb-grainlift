#!/usr/bin/env bash
# Build the Haybarn shell (Rust -> wasm) from a haybarn-wasm checkout into
# shell-build/, detached from haybarn-wasm's Cargo workspace so that tree is
# not modified. Needs rustup target wasm32-unknown-unknown; uses haybarn-wasm's
# wasm-pack.
set -euo pipefail
cd "$(dirname "$0")"
HAYBARN_WASM_DIR="${HAYBARN_WASM_DIR:-$HOME/Development/haybarn/haybarn-wasm}"
SHELL_DIR="$HAYBARN_WASM_DIR/packages/duckdb-wasm-shell"

rm -rf shell-build
mkdir -p shell-build
cp -R "$SHELL_DIR/crate" shell-build/crate
cp -R "$SHELL_DIR/src" shell-build/src
rm -rf shell-build/crate/pkg shell-build/crate/target
sed -i.bak '/^workspace = /d' shell-build/crate/Cargo.toml && rm shell-build/crate/Cargo.toml.bak
cp "$HAYBARN_WASM_DIR/Cargo.lock" shell-build/crate/Cargo.lock
(cd shell-build/crate && "$HAYBARN_WASM_DIR/node_modules/.bin/wasm-pack" build --target web --out-dir ./pkg --out-name shell --release)
