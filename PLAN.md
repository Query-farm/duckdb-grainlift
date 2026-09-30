# adbc_grainlift_wasm: plan

A DuckDB extension derived from `~/Development/adbc_scanner` that supports only the
**grainlift** ADBC driver. The driver (Rust, `~/Development/grainlift`) is **statically
linked**, so no dynamic driver loading is needed. On WASM (`~/Development/haybarn/haybarn-wasm`)
the vgi-rpc HTTP stack (`~/Development/vgi-rpc-rust`) is replaced with calls into DuckDB's
`HTTPUtil`, which haybarn-wasm implements with sync XHR. This is the same pattern
`~/Development/vgi` uses. The result is one extension that can reach any grainlift-exposed
ADBC driver over HTTP(S), and over iroh where the browser runtime allows it.

---

## 0. Decisions (2026-09-28)
- **Extension name:** `grainlift` (init symbol `grainlift_duckdb_cpp_init`, `ATTACH … (TYPE grainlift)`).
- **SQL functions:** all renamed from `adbc_*` to `grainlift_*` (table in Phase 0).
- **Upstream:** vgi-rpc-rust and grainlift changes land in those repos directly (no carried patches).
- **Primary target:** the **COI build (`wasm_threads`)** of haybarn / haybarn-wasm (Query.Farm's fork of
  duckdb-wasm). `wasm_eh` is a secondary target and `wasm_mvp` is out of scope. Because COI has
  SharedArrayBuffer, iroh is in scope for the main target, not a stretch goal.

## 1. Target architecture

```
haybarn-wasm engine (MAIN_MODULE, HTTPWasmUtil installed via config.SetHTTPUtil)
└── grainlift.duckdb_extension.wasm  (static lib → emcc -sSIDE_MODULE=2 post-link)
    ├── C++  (fork of adbc_scanner)
    │   ├── adbc_scan / adbc_execute / adbc_insert / catalog fns / ATTACH (TYPE grainlift)
    │   ├── adbc_static_driver.cpp   ← replaces the ADBC driver manager: global Adbc* fns
    │   │                              forward to an AdbcDriver table filled once by
    │   │                              AdbcDriverGrainliftInit()
    │   └── host_http.cpp            ← extern "C" callback: (method,url,headers,body) →
    │                                  HTTPUtil::Get(db).Request(...)  → (status,headers,body)
    └── Rust staticlib  libadbc_driver_grainlift.a  (wasm32-unknown-emscripten)
        └── vgi-rpc-client (http feature, NO reqwest) → HttpBackend::Host(executor)
                                                        └── calls host_http callback via FFI
```

Everything on the HTTP path is already synchronous request/response. vgi-rpc-client is
fully blocking. Grainlift's HTTP result reader pulls one batch per POST using
continuation tokens (`resume_stream` / `next_with_token`, grainlift `lib.rs:1425-1477`).
Sync XHR therefore fits without async, JSPI, or threads.

---

## 2. Key findings from the research

### adbc_scanner (the base)
- The only driver load point is `CreateConnectionFromOptions()` (`src/adbc_connection.cpp:18-113`).
  It sets `driver`/`entrypoint`/`profile`/search paths, then calls `AdbcDatabaseInit` (`:105`).
  Both `adbc_connect` (`src/adbc_functions.cpp:66`) and ATTACH (`src/storage/adbc_storage.cpp:72`) go through it.
- All ADBC calls sit in the RAII wrappers in `src/include/adbc_connection.hpp`. It uses about 30
  ADBC 1.1 functions, plus `AdbcStatusCodeMessage` and `AdbcErrorGetDetail{Count,}`.
  Those last three are driver-manager functions (`adbc_utils.hpp:14,77,79`).
- Dependencies: arrow-adbc 24 driver manager (vcpkg overlay, which pulls in toml++, fmt,
  `std::filesystem`, and `dlopen`), nanoarrow, toml++. There is no Arrow C++ dependency.
  Results go to DuckDB through `ArrowTableFunction::ArrowToDuckDB`.
- **Threads:**
  - `adbc_insert` (`src/adbc_insert.cpp:66-96,210`) and the ATTACH INSERT/CTAS path
    (`AdbcStreamingInsertConsumer`, `src/include/adbc_insert_stream.hpp:216-298`) run
    `BindStream`/`ExecuteUpdate` on a `std::thread`, fed through a bounded queue with condition variables.
  - **This deadlocks without threads.** It must be rewritten for WASM.
  - Everything else is single-threaded (`MaxThreads()=1`), and its mutexes are harmless.
- Filesystem-dependent pieces to drop: `adbc_profiles()`, manifests, search paths, and the
  `driver`/`entrypoint` options and secret fields.

### grainlift (the driver)
- The driver crate is `crates/adbc-driver-grainlift`, with `crate-type = ["cdylib","rlib"]` and no `staticlib`.
- It exports `AdbcDriverGrainliftInit`, plus a fallback `AdbcDriverInit`, via `adbc_ffi::export_driver!`
  (`lib.rs:1867`). It is ADBC 1.1 only and uses arrow-rs 59. `adbc_core`/`adbc_ffi` come from apache/arrow-adbc git rev `616acfd`.
- Transport is a closed enum `RemoteTransport { Http, Byte }` (`lib.rs:927-1029`):
  - HTTP uses `reqwest::blocking::Client` (`:942-956`), wrapped by `build_client` into
    `vgi_rpc_client::HttpClient::builder().client(reqwest)` (`:1666-1684`).
  - Byte covers tcp, tls+tcp and iroh. It spawns a thread per result stream (`ByteReader`, `:1297-1367`).
  - Iroh builds a multi-thread tokio runtime (`iroh_pool.rs:168-181`).
- **No Cargo features exist.** reqwest, rustls (ring), tokio full, and iroh 1.1 are all unconditional.
  The workspace also enables vgi-rpc `["http","jwt-jsonwebtoken","otel","tcp-mtls"]` for everything,
  including `grainlift-protocol`, which needs only `macros`. That drags in axum, aws-lc-rs and similar.
- The grainlift server speaks iroh raw ALPN `vgi-rpc/arrow-mux/1` (not `iroh-http/2`).

### vgi-rpc-rust (the RPC client)
- `vgi-rpc-client/src/http.rs` is blocking, and all traffic funnels through the **private**
  `HttpBackend::execute` (`:85-222`). The enum already has two variants, `Reqwest` and `Iroh(HttpiExecutor)`.
  The Iroh variant has exactly the neutral shape needed:
  `(method, path, Vec<(hdr,val)>, body) → (status, Vec<(hdr,val)>, body)` (`httpi.rs:394-419`).
- Other HTTP egress:
  - `ClientHttpFetcher`, for external-location GETs with manual redirects (`:299-455`).
    `vgi_rpc::external::Fetcher` is a public trait for this.
  - `put_external_body`, a PUT to a pre-signed URL (`:1468-1487`).
- reqwest types (`HeaderMap`, `StatusCode`, `Method`) leak throughout `http.rs` and into the
  public API (`HttpClientBuilder::client(ReqwestClient)`, `:512`), and grainlift calls that API.
- Requests used:
  - `OPTIONS health` (capability probe)
  - `POST {route}`, `/init`, `/exchange`, `__upload_url__/init`
  - `DELETE __session__`
  - `PUT` (upload) and `GET` (external fetch)
- Compression: requests are zstd-compressed, and the client decodes zstd/gzip responses itself.
  **The server already supports browser-safe negotiation via `X-VGI-Accept-Encoding` /
  `X-VGI-Content-Encoding`** (`vgi-rpc/src/http.rs:117,123`). Its CORS preflight allows those
  headers and exposes the `VGI-*` headers (`:2440-2480`).
- `vgi-rpc` core already has wasm gates (mio excluded, `process::id` guarded).
  `vgi-rpc-client` has none.

### vgi (reference for HTTPUtil in WASM)
- Pattern (`src/vgi_http_client.cpp:389-413`):
  1. `HTTPUtil::Get(db)`
  2. `InitializeParameters(context|db, url)`
  3. `PostRequestInfo(url, headers, *params, body, len)` with `try_request = true`
  4. `http_util.Request(post)`
  5. Read the body from **`post.buffer_out`**, and the status and headers from the `HTTPResponse`.
- There is **no OPTIONS** in haybarn-wasm's DuckDB `HTTPClient`. vgi probes capabilities with HEAD/GET instead.
- In WASM the browser transparently decompresses standard `Content-Encoding`. vgi therefore only
  decodes when `X-VGI-Content-Encoding` is present, and skips the Content-Length checks.
- Params must come from `InitializeParameters`, because `HTTPWasmClient` does `params.Cast<HTTPFSParams>()`.
- WASM does not require httpfs (`HTTPWasmUtil` is built into the engine). Native builds need httpfs loaded.
- No Rust is linked into vgi's wasm build, but lindel, a5, inflector and evalexpr_rhai do link Rust (see below).

### haybarn-wasm (the runtime)
- Emscripten **5.0.7**, wasm32 only.

  | Flavor | Platform | Exceptions | Threads |
  |---|---|---|---|
  | mvp | `wasm_mvp` | legacy | none |
  | eh | `wasm_eh` | `-fwasm-exceptions` | none |
  | coi | `wasm_threads` | `-fwasm-exceptions` | pthreads, pool of 8; needs COOP/COEP |
- Extensions:
  - Are side modules fetched from `<repo>/<rev>/<platform>/<name>.duckdb_extension.wasm`.
  - Are signature-checked unless `allowUnsignedExtensions: true`.
  - Have the init symbol `<name>_duckdb_cpp_init`.
- **Rust linking:**
  - The extension's loadable target is a static lib, and the `.wasm` comes from a post-build
    `emcc … -sSIDE_MODULE=2` step. **The Rust `.a` must be listed in `LINKED_LIBS` in
    `extension_config.cmake`, because `target_link_libraries` is ignored for the wasm link.**
    Follow `~/Development/a5geo/CMakeLists.txt:8-12,56-64` (the corrected pattern; lindel's pattern hijacks native builds).
  - For `wasm_eh`/`wasm_threads`, Rust must be **nightly `-Z build-std=std,panic_abort`**. Stable std
    uses legacy EH imports and fails on eh with `TypeError: c is not a function`.
  - `wasm_threads` additionally needs `+atomics,+bulk-memory,+mutable-globals`.
  - Check a build with `wasm-objdump -x x.wasm | grep -cE '<env\.(invoke_|__resumeException|getTempRet0)'`, which must print 0.
- **HTTP (`lib/src/http_wasm.cc`):**
  - Sync `XMLHttpRequest` supporting GET/HEAD/DELETE/POST/PUT, binary bodies, custom
    headers, and response headers (subject to CORS expose rules).
  - No streaming or cancellation, and `http_timeout` is ignored.
  - CORS failures come back as a fake 404.
  - **Node has no XHR**, so HTTP can only be exercised in a real browser.
- **Iroh in the browser:**
  - Haybarn's `irohAdapterWorker` (from `vgi-rpc-iroh-browser/js/adapter-worker.ts`) handles both
    `iroh://` raw `arrow-mux/1` streams and `httpi://`.
  - It uses SharedArrayBuffer rings exposed to side modules as `vgi_wasm_slot_{open,write,write_eos,read,terminal_error,release}`
    (`lib/js-stubs.js:324-485`).
  - **COI (`wasm_threads`) only.**

---

## 3. Work plan

Order: native static build first to prove the linking and shim with the existing reqwest
transport, then the HTTP abstraction, then WASM, then iroh.

### Phase 0: bootstrap this repo from adbc_scanner
1. `git init`. Copy `src/`, `test/`, `CMakeLists.txt`, `Makefile`, `extension_config.cmake`,
   `vcpkg.json` and `vcpkg-overlay/` from adbc_scanner, but not `build/` or `vcpkg/`.
2. Add submodules:
   - **DuckDB: the haybarn engine fork at the revision haybarn-wasm uses.**
     The adbc_scanner `duckdb` is `v1.5.5-13283…` and must match the engine exactly,
     or indirect-call signature mismatches follow.
   - `haybarn-extension-ci-tools` as `extension-ci-tools`.
3. Rename the extension to `grainlift`. That gives the `grainlift_duckdb_cpp_init` symbol and
   `ATTACH … (TYPE grainlift)`. Rename the sources and classes too (`adbc_scanner_extension.cpp` →
   `grainlift_extension.cpp`, `AdbcScannerExtension` → `GrainliftExtension`).
4. Remove:
   - `adbc_profiles`
   - the profile, manifest and search-path code
   - the `driver`/`entrypoint` options and secret fields
   - `ExtractDriverName` prefixes
   - the toml++ dependency
5. Rename the SQL surface to `grainlift_*` with grainlift semantics:

   | adbc_scanner | grainlift |
   |---|---|
   | `adbc_connect` | `grainlift_connect(uri, target := …, bearer_token := …, secret := …, <extra options>)` |
   | `adbc_disconnect` / `adbc_commit` / `adbc_rollback` / `adbc_set_autocommit` | `grainlift_disconnect` / `grainlift_commit` / `grainlift_rollback` / `grainlift_set_autocommit` |
   | `adbc_scan` / `adbc_scan_table` | `grainlift_scan` / `grainlift_scan_table` |
   | `adbc_execute` | `grainlift_execute` |
   | `adbc_insert` | `grainlift_insert` |
   | `adbc_info` / `adbc_tables` / `adbc_table_types` / `adbc_columns` / `adbc_schema` | `grainlift_info` / `grainlift_tables` / `grainlift_table_types` / `grainlift_columns` / `grainlift_schema` |
   | `adbc_clear_cache` | `grainlift_clear_cache` |
   | `adbc_profiles` | removed |

   - `ATTACH 'grainlift+https://host/…' AS x (TYPE grainlift, TARGET 'pg')`.
   - Secret type `grainlift` holding `uri`, `target` and `bearer_token`.
   - Rename settings, error-message prefixes, docs, and the sqllogictests to match.
   - The internal C++ `Adbc*` wrapper classes can keep their names, because they wrap the ADBC C API.

### Phase 1: statically link grainlift natively
1. grainlift: add `"staticlib"` to the driver's crate-type, or add a thin `adbc-driver-grainlift-static`
   crate that re-exports it, so the cdylib build is untouched.
2. Replace the ADBC driver manager with **`src/adbc_static_driver.cpp`** (~150 lines):
   - Hold a `static AdbcDriver g_driver`, initialized once with `AdbcDriverGrainliftInit(ADBC_VERSION_1_1_0, &g_driver, &err)`.
   - Define the global `AdbcDatabaseNew/SetOption/Init/…`, `AdbcConnection*` and `AdbcStatement*`
     functions to forward to `g_driver`, setting `private_driver`.
   - Implement `AdbcStatusCodeMessage` and `AdbcErrorGetDetail{Count,}` (forward to `g_driver.ErrorGetDetail*`).
   - Why not keep the driver manager: its `AdbcDriverManagerDatabaseSetInitFunc` hook would work,
     but it drags `dlopen`, `std::filesystem`, toml++ and fmt into wasm for nothing.
     This keeps `adbc_connection.hpp` almost unchanged (delete the `SetLoadFlags`/search-path wrappers).
   - Symbol check: DuckDB itself only exports `duckdb_adbc_init` plus a global `SetError`
     (`src/common/adbc/adbc.cpp:39`). The rest of its ADBC code is namespaced, so there are no clashes.
     The overlay's `SetError→AdbcDmSetError` rename becomes unnecessary once the driver manager is gone.
3. CMake: Corrosion `corrosion_import_crate(MANIFEST_PATH …/grainlift/Cargo.toml CRATES adbc-driver-grainlift-static)`.
   Link natively via `target_link_libraries`, and later list the `.a` in `LINKED_LIBS` for wasm.
   Add grainlift as a git submodule pinned to a rev. Its own `[patch]` already pins vgi-rpc-rust by rev,
   so bumping grainlift after each upstream vgi-rpc-rust change keeps both in step. During development,
   use a local `.cargo/config.toml` `[patch]` to point at `~/Development/grainlift` and
   `~/Development/vgi-rpc-rust`.
4. Test natively against a local `grainlift-server` with a DuckDB or sqlite target. Port adbc_scanner's
   sqllogictests from `driver=…` connections to `grainlift+http://127.0.0.1:…` plus `target`.

### Phase 2: pluggable HTTP backend in vgi-rpc-rust (upstream PRs)
1. `vgi-rpc-client`:
   - Split the features: `http` (protocol logic: zstd, flate2, base64, sha2, `vgi-rpc/external`) and
     `reqwest` (backend, in the default set).
   - Replace reqwest's `HeaderMap`/`HeaderName`/`HeaderValue`/`StatusCode`/`Method` with the pure-Rust
     `http` crate (reqwest re-exports those same types, so this is mostly import churn).
     Replace `reqwest::Url` with `url::Url`.
2. Add a public executor trait:
   ```rust
   pub struct HttpRequest<'a> {
       pub method: &'a str,
       pub url: &'a str,
       pub headers: &'a [(String, String)],
       pub body: &'a [u8],
       pub timeout: Duration,
       pub follow_redirects: bool,
   }
   pub struct HttpResponse {
       pub status: u16,
       pub headers: Vec<(String, String)>, // repeated names allowed
       pub body: Vec<u8>,
   }
   pub trait HttpExecutor: Send + Sync {
       fn execute(&self, req: HttpRequest<'_>) -> Result<HttpResponse, HttpExecError /* msg + retry_safe */>;
       fn capabilities(&self) -> ExecutorCaps {
           ExecutorCaps::default() // supports_options, transparent_decompression
       }
   }
   ```
   Wire it up as follows:
   - Add `HttpBackend::Custom(Arc<dyn HttpExecutor>)` and `HttpClientBuilder::executor(...)`.
   - Make `build_with_backend` generic over the external client.
   - Route `put_external_body` through the executor.
   - Implement `vgi_rpc::external::Fetcher` on top of the executor so external-location GETs use it too.
3. Make browser-transport behaviour conditional on `ExecutorCaps`:
   - **`supports_options = false`:** probe capabilities with `GET`/`HEAD health` instead of `OPTIONS`.
     Confirm the server stamps capability headers on non-OPTIONS responses (`attach_capability_headers`, `vgi-rpc/src/http.rs:2129`).
   - **`transparent_decompression = true`:**
     - Send `X-VGI-Accept-Encoding: zstd, gzip`, never `Accept-Encoding` (a forbidden header in browsers).
     - Decode only when `X-VGI-Content-Encoding` is present.
     - Treat a standard `Content-Encoding` as already decoded, and skip Content-Length checks.
     - Request-body zstd compression stays as-is, because it is a request header.
4. Build the client for `wasm32-unknown-emscripten` with `--no-default-features --features http`:
   - Verify `zstd-sys` builds with emcc.
   - Verify `socket2`/`libc` under `cfg(unix)` compile (emscripten counts as unix).
   - `SubprocessTransport`, `TcpTransport` and `PipeTransport` compile but must never be used on wasm.
     Optionally `cfg`-gate them out.
5. Add a CI job that runs `cargo build -p vgi-rpc-client --target wasm32-unknown-emscripten --no-default-features --features http`.

### Phase 3: make grainlift's driver wasm-buildable
1. Features on `adbc-driver-grainlift`:
   - `default = ["reqwest-http", "byte-transports", "iroh"]`
   - `host-http`, for the executor supplied through FFI
   - `iroh-browser`, for phase 6
2. Workspace deps:
   - Set `grainlift-protocol` to use `vgi-rpc` with `default-features=false, features=["macros"]`.
   - Move `http`/`jwt`/`otel`/`tcp-mtls` into `grainlift-server`'s own dependency line.
   - Make `tokio`, `iroh`, `rustls`, `reqwest` and `vgi-rpc-iroh` optional in the driver.
3. `RemoteTransport`:
   - `cfg`-gate the `Byte` variant and `ByteConnector`/`ByteReader`/`iroh_pool`/`tls_tcp_client`.
   - Gate `read_certificates` and the `grainlift.tls.*` file options behind `byte-transports`/`reqwest-http`
     (on wasm the browser owns TLS).
   - Gate the `grainlift.iroh.*` options behind `iroh`.
   - Keep `bearer_token`, `request_timeout_ms` and `max_response_bytes`. Default `max_response_bytes`
     to 64 MiB on wasm, matching vgi.
4. **Host HTTP FFI.** Rust exports the entry points and C++ supplies the callback:
   ```c
   // Rust side; declared in the extension's grainlift_host.h
   typedef int (*grainlift_http_fn)(
       void *host_ctx, const char *method, const char *url,
       const char *const *hdr_names, const char *const *hdr_values, size_t n_hdrs,
       const uint8_t *body, size_t body_len, uint32_t timeout_ms, int follow_redirects,
       struct grainlift_http_response *out);           // C++ allocates, Rust copies
   void grainlift_register_host_http(grainlift_http_fn fn, void (*free_resp)(struct grainlift_http_response *));
   ```
   - Pass `host_ctx` per database as an internal ADBC database option (`grainlift.internal.host_ctx`),
     set by the extension before `AdbcDatabaseInit`. It points at a C++ object holding a
     `weak_ptr<DatabaseInstance>` and cached `HTTPParams`.
   - Registering a function pointer avoids unresolved-import problems in native test binaries.
     It also lets the native build choose reqwest or host HTTP at runtime.
5. Panics:
   - With `panic_abort` std on eh/threads, `catch_unwind` in `adbc_ffi` is inert, so a panic kills the instance.
   - Set `panic = "abort"` in a `[profile.wasm]`.
   - Audit `unwrap()`s on the HTTP path.
6. Size: build with `opt-level = "z"`, `lto = "fat"`, `codegen-units = 1`, and strip debuginfo.
   Measure arrow-rs 59 plus the IPC reader in the `.wasm`.

### Phase 4: C++ extension side for WASM
1. **`src/host_http.cpp`**, implementing the callback:
   - Get the util with `HTTPUtil::Get(*db)`.
   - Get params with `InitializeParameters(*db, url)` and cache them per host_ctx (vgi `GetOrInitHttpParams`
     explains why: this avoids a secret-manager/MetaTransaction deadlock).
   - Set `params->timeout` and `follow_location`.
   - Build the matching request info:
     - `GetRequestInfo` with a `content_handler` that appends to a buffer
     - `PostRequestInfo` / `PutRequestInfo` with the body
     - `HeadRequestInfo`
     - `DeleteRequestInfo`
   - Set `try_request = true`.
   - Read the body from `buffer_out` for POST/PUT and the handler buffer for GET.
     Map `HasRequestError()` to an error with `retry_safe`.
   - Copy the response headers, including repeats where available.
   - Register it with `grainlift_register_host_http` in `LoadInternal`.
   - Natively this needs httpfs loaded (autoload it, as vgi does at `src/vgi_extension.cpp:2049`).
     In WASM, `HTTPWasmUtil` is always present.
   - **Using the host HTTP path natively too gives a single code path that sqllogictests can exercise**,
     which matters because Node has no XHR.
2. **Single-threaded inserts:** under `__EMSCRIPTEN__`, or always if simpler, replace the producer thread
   in `adbc_insert` and `AdbcStreamingInsertConsumer`. Accumulate the `ArrowAppender` batches, then in
   Finalize call `BindStream` (with a stream over the buffered batches) and `ExecuteUpdate` synchronously.
   Add a memory cap and error when it is exceeded. A later option is multiple ingest calls with `mode=append`.
3. Telemetry: drop it, or keep the existing `__EMSCRIPTEN__` synchronous branch but route it through `HTTPUtil`.
4. The existing `AdbcOperationLease`, mutexes and `MaxThreads()=1` are fine as they are.

### Phase 5: WASM build and packaging
1. Toolchain:
   - emsdk 5.0.7.
   - Rust nightly (`nightly-2026-05-20`, the pin haybarn CI uses) plus `rust-src`, with
     `CARGO_UNSTABLE_BUILD_STD=std,panic_abort`.
   - For `wasm_threads`, `RUSTFLAGS=-Ctarget-feature=+atomics,+bulk-memory,+mutable-globals`.
   - Use stable Rust for `wasm_mvp` only if that flavor is supported (legacy EH on both sides).
2. `CMakeLists.txt`:
   - Set `Rust_CARGO_TARGET=wasm32-unknown-emscripten` only when `DUCKDB_EXPLICIT_PLATFORM` matches `wasm*`
     (the a5geo pattern).
   - Pass `--no-default-features --features host-http`.
   - In `extension_config.cmake`:
     `duckdb_extension_load(grainlift SOURCE_DIR … LINKED_LIBS "../../cargo/build/wasm32-unknown-emscripten/<profile>/libadbc_driver_grainlift_static.a")`.
3. vcpkg: only nanoarrow remains (or vendor the nanoarrow amalgamation and drop vcpkg entirely, which is simpler for wasm).
4. Build **`wasm_threads` (COI) first**, using a `build-wasm-coi.sh` modeled on vgi's (same env vars:
   `EMSDK_DIR`, `DUCKDB_WASM_DIR=~/Development/haybarn/haybarn-wasm`, `DEPLOY_DIR`):
   - Extension CXX flags: `-fwasm-exceptions -msimd128 -mbulk-memory -pthread -sUSE_PTHREADS=1 -sSHARED_MEMORY=1 -DWEBDB_THREADS=1`.
   - `-DUSE_WASM_THREADS=TRUE` so the post-link adds `-pthread -sSHARED_MEMORY=1`.
   - vcpkg triplet `wasm32-emscripten-threads`, if vcpkg is still used.
   - Rust: build-std **plus** `+atomics,+bulk-memory,+mutable-globals`. Mixing an atomics-less Rust archive into a
     shared-memory side module fails at link time or corrupts memory.

   Build `wasm_eh` second (no Rust target features, no `-pthread`). Skip `wasm_mvp`.
5. Gates:
   - `wasm-objdump` shows 0 `invoke_`/`__resumeException` imports.
   - No unexpected `env.*` imports (e.g. from zstd-sys or socket shims).
   - The `.wasm` size is recorded.

### Phase 6: testing
1. **Native:**
   - Run sqllogictests against `grainlift-server`, spawned by a test harness script, with both the
     reqwest and host-HTTP backends.
   - Unit tests in vgi-rpc-client for the executor: a mock executor, the OPTIONS→GET fallback,
     and `X-VGI-Content-Encoding` handling with transparent decompression.
2. **Browser:**
   - Serve `build/…/repository` locally with CORS **and CORP/COEP-compatible headers** (the COI page is
     cross-origin isolated, so the extension fetch needs `Cross-Origin-Resource-Policy: cross-origin`) as
     `<repo>/<rev>/wasm_threads/grainlift.duckdb_extension.wasm`.
   - The haybarn-wasm app must be served with COOP/COEP (`packages/duckdb-wasm-app/static/_headers`,
     `webpack.debug.js`) so the COI bundle is selected (`packages/duckdb-wasm/src/platform.ts:113`).
   - The grainlift server's responses also need `Cross-Origin-Resource-Policy` (or CORS, which is enough for
     XHR under `require-corp`); confirm this in the browser.
   - Open the haybarn-wasm shell with `allowUnsignedExtensions: true`, then run `INSTALL grainlift FROM 'http://127.0.0.1:8080/repo'; LOAD grainlift;`.
   - Drive it with Playwright.
   - `grainlift-server` must have CORS enabled:
     - allow `Authorization`, `Content-Type`, `X-VGI-Accept-Encoding`, `VGI-*`
     - expose `VGI-*` and `X-VGI-Content-Encoding`
     - answer preflights on every route
3. Scenarios:
   - `grainlift_connect` + `grainlift_scan`
   - ATTACH + `SELECT`, including filter pushdown
   - `grainlift_execute`
   - INSERT via ATTACH
   - a large result spanning many continuation POSTs
   - auth failure (401)
   - a CORS-misconfigured server (the fake 404 should give a clear error message)
   - a response over the 64 MiB limit
   - on COI: the same queries with `SET threads=4`, to confirm the scan and HTTP bridge behave when DuckDB
     worker pthreads (not the main engine worker) make the sync XHR calls
   - `iroh://` against a grainlift-server with iroh enabled (Phase 7)

### Phase 7: iroh on WASM (COI; part of the main deliverable)
- Implement a vgi-rpc-client `Transport` (Read/Write over a byte duplex) on top of the Haybarn SAB
  ring stubs `vgi_wasm_slot_*`, as `extern "C"` imports resolved from the engine's `js-stubs.js`.
  Open an `iroh://<endpoint-id>` slot through the page's `irohAdapterWorker`. That speaks `arrow-mux/1`,
  which is what `grainlift-server` already serves.
- Make grainlift's `ByteReader` synchronous under wasm (no per-stream `thread::spawn`).
  It should pull the next batch on demand, because `pthread_create` after `dlopen` is unreliable.
- Identity: the browser node owns the iroh secret key, so `grainlift.iroh.secret_key*` is ignored or rejected on wasm.
- `wasm_eh` has no SAB, so there `iroh://` fails with a clear "requires the threads (COI) build" error.
- Follow vgi's lead on shared-memory ring use (`src/vgi_webworker_function_connection.cpp`,
  `docs/sab_transport_abi.md`): re-publish the channel offset before each ring operation, and read
  `wasmMemory.buffer`, not a stale `HEAPU8`.
- The host page must install the iroh adapter worker (`irohAdapterWorker` option;
  `@haybarn/haybarn-wasm >= 1.5.5-rc4`).
- Confirm the shared-memory stub signatures against the current `js-stubs.js` and vgi's C++ caller, and
  write the Rust `extern "C"` declarations to match. Consider a tiny shared C header both repos use.

---

## 4. Risks and gotchas
- **Engine ABI match:**
  - Build against the exact DuckDB revision of the target haybarn-wasm release, otherwise indirect-call signature mismatches follow.
  - The ADBC 1.1 struct layout must also match what `adbc_ffi` produces. Both sides come from arrow-adbc, so pin compatible versions.
- **CORS** is the most likely source of "works natively, fails in the browser". Every `VGI-*` response header the client reads
  (for example `VGI-Accept-Max-Response-Bytes-Support`, which is mandatory on POST) must be in
  `Access-Control-Expose-Headers`, or the client rejects the response.
- **Sync XHR blocks the engine worker:**
  - There is no cancellation, and `http_timeout` has no effect.
  - Keep `adbc.statement.batch_size` moderate so each continuation POST stays small.
- **Response size:** 64 MiB default on wasm; the browser buffers each response fully.
- **Panics are fatal** on eh/threads because std is built with `panic_abort`.
- **COI threading:** `pthread_create` after `dlopen` is unreliable (emsdk #19425/#19199/#13303), and the
  engine pre-sizes a pool of 8. The extension and the Rust code must not spawn threads. That rules out the
  insert producer thread, grainlift's `ByteReader` thread, the tokio runtime and reqwest's blocking runtime,
  which are all removed or gated in the plan. DuckDB's own worker threads may call into grainlift
  concurrently on different connections; the driver must stay `Send + Sync`-correct, which it already is natively.
- **Duplicate symbols** in the final link: zstd (DuckDB's copy is namespaced, so this is probably fine), and the
  `AdbcDriverInit` fallback export (harmless as long as nothing else defines it).
- **Binary size** of arrow-rs plus Rust std in a side module. Measure early in Phase 5.
- **Toolchain drift:** nightly Rust and build-std are pinned by haybarn CI, and emsdk must be exactly 5.0.7.

## 5. Open questions
1. **Native build:** should it also ship (host-HTTP backend, requiring httpfs), or is it only a test vehicle for the WASM target?
2. **Distribution:** publish through the haybarn community extensions repo (signed), or host it privately?

---

## 6. Status (2026-09-28): implemented and tested

All phases are implemented. The extension builds natively, for `wasm_threads`
(COI) and for `wasm_eh`, and was tested in a browser against a local
grainlift-server over both HTTP and Iroh.

| Repo | Branch / location | Change |
| --- | --- | --- |
| vgi-rpc-rust | `feat/http-executor` (worktree `~/Development/vgi-rpc-rust-wasm`), commit `2b3d548` | `HttpExecutor` trait + `HttpClientBuilder::executor`; `reqwest` split into its own feature; OPTIONS→GET probe and `X-VGI-*-Encoding` when the transport decompresses; builds for wasm32-unknown-emscripten |
| grainlift | `feat/wasm-host-http` (worktree `~/Development/grainlift-wasm`), commit `32df1b5` | Driver features (`reqwest-http`, `tls-tcp`, `iroh`, `host-http`, `iroh-browser`), staticlib, host HTTP C ABI, SAB Iroh transport + thread-free reader, pooled VGI HTTP clients, `server.cors_origins` |
| adbc_grainlift_wasm | this repo (not yet committed) | The extension |

Neither branch is pushed. grainlift's `[patch.crates-io]` points at the local
vgi-rpc-rust worktree; switch it to the pushed git rev before merging.

### Deviations from the plan

- The internal C++ files/classes keep their `adbc_*` names (they wrap the ADBC C
  API; this keeps adbc_scanner fixes easy to port). All SQL names are `grainlift_*`.
- No vcpkg for WASM: `adbc.h` and a namespaced nanoarrow bundle are vendored in
  `third_party/`. Natively vcpkg only supplies openssl/curl for httpfs.
- Query.Farm telemetry was dropped.
- The iroh path uses the raw `arrow-mux/1` protocol through Haybarn's adapter
  Worker (what grainlift-server already serves), not `httpi://`.
- `grainlift_connect` prepares `iroh://` targets while binding: the adapter
  Worker can only be requested from DuckDB's main worker thread, and scalar
  functions may execute on a pthread.
- Grainlift pulls every input batch during `BindStream`, so the ATTACH insert
  path now binds on the consumer thread (natively) or at finalize (WASM),
  never on the producer.

### Test results

- Native sqllogictests (`test/sql`, 14 files, against grainlift-server with
  sqlite targets): all pass, with both the threaded and the thread-free
  (`-DGRAINLIFT_NO_INSERT_THREAD=ON`, used by WASM) insert paths.
- vgi-rpc-rust: all client/server tests, clippy and fmt pass (see its commit).
- grainlift: driver/protocol/server tests pass. (Four `result_reuse` TCP tests
  used to fail on macOS only: accepted sockets inherited the listener's
  `O_NONBLOCK`; fixed in grainlift#16.)
- Browser (Playwright, `@haybarn/haybarn-wasm@1.5.5-rc6`, engine `105edd31b5`):
  - COI: INSTALL/LOAD from a local repository; `grainlift_connect`/scan/info/
    execute/disconnect; ATTACH with pushed-down filters and joins; CTAS and
    INSERT; 100k-row and 800k-row streamed scans; `SET threads = 4`; errors for
    bad token, unauthorized target, unreachable service, bad SQL.
  - COI over `grainlift+iroh://` via relay: connect, scans (100k rows), info,
    ATTACH, CTAS/INSERT, execute.
  - eh: HTTP scan/ATTACH/CTAS; `iroh://` fails with a clear "needs COI" error.

- PostgreSQL (added later): `test/sql/grainlift_postgres.test` (149 assertions)
  passes natively, and the page's 18 in-browser PostgreSQL checks pass on COI
  over both HTTP and Iroh. This found and fixed an inherited bug: ATTACH
  INSERT/CTAS did not pass `adbc.ingest.target_db_schema`, so rows landed in
  the remote default schema.

### Known gaps / follow-ups

- The ADBC PostgreSQL driver returns `numeric` as VARCHAR; cast in SQL.
- An attached PostgreSQL database has no `main` schema; qualify `public` tables
  (`pg.public.t`).

- ~~DDL through ATTACH~~ (fixed 2026-09-30): `CREATE TABLE` (via ADBC
  ingestion of an empty stream, so the driver writes dialect-correct DDL),
  `CREATE [OR REPLACE | IF NOT EXISTS] TABLE [AS]` and `DROP TABLE/VIEW` work;
  constraints, defaults and generated columns are rejected with a pointer to
  `grainlift_execute`.
- PostgreSQL `numeric` columns read as VARCHAR (the ADBC PostgreSQL driver
  returns `arrow.opaque` over utf8; upstream apache/arrow-adbc#4798 adds
  `POSTGRESQL:typmod`), and consequently INSERT into a `numeric` column through
  ATTACH fails ("COPY Writer from Arrow type 'string' to PostgreSQL numeric is
  not implemented"). Deferred.
- SQLite: the ADBC SQLite driver infers column types from row data, so an
  empty table's columns all read as BIGINT (its `GetObjects` has the declared
  types; `GetTableSchema` does not).
- ~~Mid-stream failure~~ (fixed 2026-09-30, grainlift#16 + vgi-rpc-rust#7):
  a result stream interrupted by a transport failure resumes from the next
  batch sequence (HTTP retries the continuation token; tcp/iroh reopen
  `read_result`), using the server's replay of the last batch. Only if the
  server lost the session too (Iroh connection closed, TTL) does the query
  fail, with "result stream interrupted after N batches … rerun the query".
  Byte transports previously ended such a stream silently early.
- Attached catalogs cache remote metadata; after schema changes made elsewhere
  (another attachment, `grainlift_execute`), run `CALL grainlift_clear_cache()`.
- The browser buffers each HTTP response; there is no cancellation and
  `http_timeout` is ignored by sync XHR.
- The `duckdb` submodule was cloned with `--reference ~/Development/haybarn/haybarn`
  (git alternates); run `git -C duckdb repack -a -d && rm duckdb/.git/objects/info/alternates`
  (in the submodule's gitdir) before relying on it elsewhere.
- CI (haybarn-extension-ci-tools) and signed publishing are not set up.
