# grainlift DuckDB extension

Query any database exposed by a [grainlift](../grainlift) service from DuckDB,
including DuckDB-WASM ([Haybarn](../haybarn/haybarn-wasm)) in the browser.

The extension is derived from [adbc_scanner](../adbc_scanner) but supports only
the grainlift ADBC driver, which is compiled in (DuckDB-WASM cannot load ADBC
drivers dynamically). Every HTTP request the driver makes goes through DuckDB's
`HTTPUtil` — the browser's `XMLHttpRequest` in DuckDB-WASM, httpfs natively —
and in Haybarn's cross-origin-isolated (COI) build the driver can also reach a
grainlift service over Iroh through the page's Iroh adapter Worker.

## Using it

```sql
LOAD grainlift;

-- Attach the remote database (or grainlift+iroh://<endpoint-id>)
ATTACH 'grainlift+https://grainlift.example.com' AS wh (TYPE grainlift, TARGET 'warehouse', BEARER_TOKEN '...');
SELECT count(*) FROM wh.orders WHERE status = 'open';   -- filters and projections are pushed down
CREATE TABLE wh.snapshot AS SELECT * FROM local_table;  -- bulk ingestion
CREATE TABLE wh.events (id BIGINT, name VARCHAR);        -- remote DDL, in the driver's dialect
DROP TABLE wh.events;

-- Run SQL in the remote database's own dialect, naming the attachment by its alias
SELECT * FROM grainlift_scan('wh', 'SELECT * FROM orders LIMIT 10');
CALL grainlift_execute('wh', 'CREATE INDEX orders_status ON orders (status)');
```

The `grainlift_*` functions take the attached database's alias. Inside a
`BEGIN … COMMIT` transaction, `grainlift_execute` and `grainlift_insert` join the
attachment's transaction (they commit or roll back with writes made through
`wh.…`), and reads see its uncommitted writes; otherwise they autocommit.

| Function | Purpose |
| --- | --- |
| `grainlift_scan(database, sql, params := ..., columns := ..., batch_size := ...)` | Run a query remotely |
| `grainlift_scan_table(database, table)` | Scan a table with filter/projection pushdown |
| `grainlift_execute(database, sql)` | DDL/DML |
| `grainlift_insert(database, table, (SELECT ...), mode := ...)` | Bulk ingestion |
| `grainlift_info`, `grainlift_tables`, `grainlift_table_types`, `grainlift_columns`, `grainlift_schema` | Metadata |
| `grainlift_clear_cache()` | Drop cached ATTACH metadata |

`database` is the alias of an `ATTACH … (TYPE grainlift)` database.

ATTACH options (also usable in `CREATE SECRET (TYPE grainlift, ...)`):

| Option | Meaning |
| --- | --- |
| `uri` | grainlift service: `grainlift+https://`, `grainlift+http://`, `grainlift://` (HTTPS) or `grainlift+iroh://<64-hex-endpoint-id>` |
| `target` | Server-side target name |
| `bearer_token` (`token`) | HTTP bearer token |
| `request_timeout_ms`, `max_response_bytes`, `max_bind_bytes` | Driver limits |
| `remote_uri` | The downstream driver's `uri`, when the target allows it |
| `dialect` | Override SQL dialect detection for pushdown (default: remote vendor name) |
| `secret` | Name of a `grainlift` secret to use |
| anything else | Forwarded to the downstream driver as a database option |

Secrets are matched by the `uri` scope, so `CREATE SECRET (TYPE grainlift, URI '...', TARGET '...', BEARER_TOKEN '...')`
lets `ATTACH '<uri>' AS x (TYPE grainlift)` connect with no further options.

### In the browser (Haybarn)

- The grainlift service must allow the page's origin: `server.cors_origins` in its config.
- `iroh://` needs the COI build and an Iroh adapter Worker installed by the page:

  ```js
  import { installVgiWebWorkerBridge } from '@haybarn/haybarn-wasm/vgi';
  const irohAdapter = new Worker('iroh-adapter.js', { type: 'module' });  // see browser-test/site/iroh-adapter.js
  installVgiWebWorkerBridge({ irohAdapterWorker: irohAdapter })(duckdbWorker);
  ```

  The browser node's endpoint ID must be mapped to a principal in the server's `iroh.principals`.
- Responses are buffered by the browser (sync XHR); keep `max_response_bytes` modest.

## Building

Sources: this repo and its submodules — `duckdb` (the Haybarn engine, pinned
to the revision Haybarn-WASM ships), `extension-ci-tools`, and `grainlift` (the
driver, which pins vgi-rpc-rust). `GRAINLIFT_SOURCE_DIR` overrides the grainlift
checkout.

```sh
git submodule update --init --recursive

# Native (loads httpfs for HTTP); vcpkg provides openssl/curl for httpfs.
VCPKG_TOOLCHAIN_PATH=~/Development/vcpkg/scripts/buildsystems/vcpkg.cmake GEN=ninja make release

# DuckDB-WASM, COI (wasm_threads) and eh; needs emsdk 5.0.7 and
# rustup toolchain nightly-2026-05-20 with rust-src + wasm32-unknown-emscripten.
scripts/build-wasm.sh wasm_threads
scripts/build-wasm.sh wasm_eh
```

The Rust driver is built by `cmake/grainlift_rust.cmake` (`cargo rustc
--crate-type staticlib`, features `host-http` natively and
`host-http,iroh-browser` for WASM). For WASM it uses `-Z build-std` so std has
native wasm exception handling like the engine; `build-wasm.sh` fails if any
legacy exception-handling import remains.

## Testing

```sh
# A local grainlift-server with SQLite targets (`dbc install sqlite`) and, when
# PostgreSQL's initdb is available, a throwaway PostgreSQL cluster on port 55432
# seeded from scripts/pg_seed.sql as the `postgres` target (`dbc install postgresql`).
scripts/test-server.sh 8484 http://127.0.0.1:8080

GRAINLIFT_URI=grainlift+http://127.0.0.1:8484 GRAINLIFT_TOKEN=grainlift-test-token GRAINLIFT_PG=1 \
  ./build/release/test/unittest --test-config test/configs/grainlift.json "test/sql/*"
```

`test/sql/grainlift_postgres.test` needs `GRAINLIFT_PG=1` (skipped otherwise).

Browser: `browser-test/` is a small page on the published `@haybarn/haybarn-wasm`
bundle. `npm install && npm run build`, copy the built `.wasm` files to
`browser-test/repo/v1.5.5/<platform>/`, run `python3 serve.py 8080` (COOP/COEP
headers), and open `http://127.0.0.1:8080/` (`?bundle=eh` for the eh build,
`?irohKey=<hex>` to pin the browser's Iroh identity). `window.grainliftTest.sql()`
runs SQL for automation. The page attaches the SQLite demo as `demo` and
PostgreSQL as `pg`, offers example queries, and has a **Run PostgreSQL checks**
button (`window.grainliftTest.runPostgresChecks()`) that runs assertions in the
browser. For Iroh, start the test server with
`GRAINLIFT_IROH_PRINCIPAL=<browser endpoint id>` and open
`?transport=iroh&irohServer=<server endpoint id>&irohKey=<browser key hex>`
(the server's id is in `$TMPDIR/grainlift-test-server/iroh-endpoint.json`).

### Demo: the Haybarn shell with grainlift

`browser-test/site/shell.html` is the demoable page: the Haybarn DuckDB-WASM
shell (xterm) with the grainlift extension loaded and the PostgreSQL (`pg`) and
SQLite (`demo`) targets attached at startup, a live connection panel, and a
guided list of queries whose **Run** button types them into the shell. The
header switches between HTTP and Iroh.

```sh
cd browser-test
./build-shell.sh          # once: builds the shell wasm from ~/Development/haybarn/haybarn-wasm
npm install && npm run build
python3 serve.py 8080     # then open http://127.0.0.1:8080/shell.html
```

For Iroh, start the test server with `GRAINLIFT_IROH=1` (plus `GRAINLIFT_OPEN=1`,
or `GRAINLIFT_IROH_PRINCIPAL=<browser endpoint id>` with `?irohKey=<hex>` to pin
the browser identity); `serve.py` publishes the server's endpoint ID so
`?transport=iroh` needs no other parameters locally.

### Hosted demo

`deploy/publish.sh` publishes the demo to
<https://grainlift-demo.query-farm.services/>: the site files and the COI
extension go to the `grainlift-demo` R2 bucket, and a small Worker
(`deploy/worker.js`) serves them with the COOP/COEP headers cross-origin
isolation needs and owns the custom domain.

The hosted page defaults to Iroh; enter the grainlift-server's Iroh endpoint ID
in the sidebar (or link `?irohServer=<id>`). To demo your local databases,
run the test server open (no authentication, Iroh enabled) and share the link
privately:

```sh
GRAINLIFT_OPEN=1 GRAINLIFT_IROH=1 scripts/test-server.sh 8484
# endpoint ID: $TMPDIR/grainlift-test-server/iroh-endpoint.json
```

`GRAINLIFT_OPEN=1` lets anyone who knows the endpoint ID use every target.

See [PLAN.md](PLAN.md) for the design.
