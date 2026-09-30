# duckdb-grainlift

DuckDB extension that queries any database exposed by a
[grainlift](https://github.com/Query-farm/grainlift) service, over HTTP(S) or
Iroh. Derived from [adbc_scanner](https://github.com/Query-farm/adbc_scanner),
but with the grainlift ADBC driver (Rust) statically linked instead of loaded
at runtime.

## Why this exists: WASM

This extension exists for **DuckDB-WASM**, where ADBC drivers cannot be loaded
dynamically (no `dlopen` of arbitrary native libraries in the browser). On
Linux, macOS and Windows people should use **adbc_scanner**, which loads any
ADBC driver in-process — including the grainlift driver itself — so a native
grainlift extension adds nothing there.

Consequences for decisions in this repo:

- **Ship for Haybarn only.** The release target is the Haybarn community
  extensions repository (`~/Development/haybarn/haybarn-community-extensions`),
  built against the Haybarn engine and served to haybarn-wasm. It is not meant
  for duckdb/community-extensions: the browser Iroh path depends on Haybarn's
  SharedArrayBuffer ring stubs (`vgi_wasm_*` in haybarn-wasm `lib/js-stubs.js`)
  and its Iroh adapter-worker bridge, and the build pins Haybarn's DuckDB fork.
- **WASM is the product; native builds are a test vehicle.** The native build
  exists so the sqllogictests can exercise the same code path (HTTP through
  DuckDB's `HTTPUtil`, via httpfs) without a browser. Don't spend effort on
  native-only features or native packaging.
- **COI (`wasm_threads`) is the primary flavor**; `wasm_eh` is supported
  (HTTP only — `iroh://` needs COI); `wasm_mvp` is out of scope.

## Architecture

- `src/adbc_static_driver.cpp` — the global `Adbc*` C API forwards to the
  statically linked `AdbcDriverGrainliftInit`; no driver manager.
- `src/grainlift_host_http.cpp` — the driver's HTTP executor (C ABI in
  `src/include/grainlift_host.h`) implemented with DuckDB `HTTPUtil`: sync XHR
  in DuckDB-WASM, httpfs natively (autoloaded).
- `iroh://` in the browser: the driver talks through Haybarn's SAB rings to the
  page-owned Iroh adapter Worker; `grainlift_connect`'s bind calls
  `grainlift_prepare_endpoint` because the Worker can only be requested from
  DuckDB's main worker thread.
- Inserts: no threads on WASM (`GRAINLIFT_INSERT_THREAD`), and the driver pulls
  the whole bind stream inside `BindStream`, so binding never runs on the
  producer thread.
- The rest (`adbc_scan`, catalog, storage/ATTACH, filter pushdown) is
  adbc_scanner's code with SQL names renamed to `grainlift_*`; internal file and
  class names keep their `adbc_*` names to ease porting fixes.

## Build and test

See README.md. In short: `git submodule update --init --recursive`; native
`make release` (vcpkg for openssl/curl); WASM `scripts/build-wasm.sh
wasm_threads|wasm_eh` (emsdk 5.0.7, Rust `nightly-2026-05-20` + rust-src,
`-Z build-std` so Rust uses native wasm EH — the script fails on legacy EH
imports). Tests need a grainlift server: `scripts/test-server.sh` (SQLite +
throwaway PostgreSQL), then the unittest runner with
`--test-config test/configs/grainlift.json`. Browser: `browser-test/`
(`shell.html` is the demo; `deploy/publish.sh` publishes it to
grainlift-demo.query-farm.services).

## Gotchas

- The `duckdb` submodule must match the haybarn-wasm engine revision exactly
  (indirect-call signature mismatches otherwise). Test against the published
  `@haybarn/haybarn-wasm` npm package; a local haybarn-wasm `dist/` may be stale.
- The Rust archive must be in `LINKED_LIBS` for WASM (`target_link_libraries`
  is ignored by the post-build `emcc -sSIDE_MODULE=2` step).
- DuckDB's test runner skips any test whose error mentions "HTTP"; always run
  with `test/configs/grainlift.json`.
- The ADBC PostgreSQL driver returns every `numeric` as `arrow.opaque`
  (vendor PostgreSQL, type numeric) over utf8, with no precision/scale anywhere
  (not in the schema, `GetObjects`, or `GetTableSchema`).
