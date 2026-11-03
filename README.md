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

The `v2.0` branch targets DuckDB 2.0.0 and Haybarn 2.0.0-rc11. The extension
version is 0.6.1; its version is independent of the engine version. Native
Linux x86-64/ARM64 and macOS ARM64 builds are signed and published to Query.Farm:

```sql
SET allow_extension_repositories = 'allowed';
INSTALL grainlift FROM query_farm;
LOAD grainlift FROM query_farm;
```

Haybarn's `wasm_threads` and `wasm_eh` builds are published through the Haybarn
community registry (`INSTALL grainlift FROM community`). Iroh requires the
Haybarn threaded engine; native builds support HTTP(S). Windows and upstream
DuckDB-WASM binaries are not published by this repository.

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
| `oauth_refresh_token` | HTTP OAuth refresh token; the driver exchanges it for bearer tokens before they expire and after a 401. The token endpoint and client ID are discovered from a gateway configured with `[auth.oauth]` |
| `oauth_token_endpoint`, `oauth_client_id`, `oauth_client_secret` | Override OAuth discovery |
| `request_timeout_ms`, `max_response_bytes`, `max_bind_bytes` | Driver limits |
| `remote_uri` | The downstream driver's `uri`, when the target allows it |
| `dialect` | Override SQL dialect detection for pushdown (default: remote vendor name) |
| `attach_secret` (`secret`) | Name of a `grainlift` secret to use; `''` turns secret lookup off for this ATTACH |
| anything else | Forwarded to the downstream driver as a database option |

### HTTP request costs

Versions 0.6.x embed the Grainlift driver and require a protocol 0.5
gateway. HTTP capabilities arrive with `open_connection`; the driver makes no
`/health` probe. When the selected backend explicitly declares statistics
unsupported, metadata planning returns `NOT_IMPLEMENTED` locally. Table schemas
are still fetched for each query. Session reopen refreshes the capability flags.

The Cloudflare demo's two-table warm join uses 21 requests instead of 25 while
retaining its two schema requests. `browser-test/profile-http.mjs` checks this
behavior, direct table scans, catalog refresh, and cold connection setup.

### Credentials from secrets

`CREATE SECRET (TYPE grainlift, URI '...', TARGET '...', BEARER_TOKEN '...')`
lets `ATTACH '<uri>' AS x (TYPE grainlift)` connect with no further options, so
the ATTACH text carries no credential. The secret is chosen once per ATTACH,
from the user's own inputs only:

1. `attach_secret '<name>'` (or `secret '<name>'`): that secret. It must exist
   and be of type `grainlift`; its scope is not checked.
2. `attach_secret ''`: no secret at all.
3. Otherwise the `grainlift` secret whose scope (default: its `URI`) best
   matches the ATTACH URI, **ending at a URL boundary**: the scope equals the
   URI, ends in `/`, or the URI continues with `/`, `?`, `#` or `:`. So
   `grainlift+https://gw.example.com` matches `…gw.example.com/db`,
   `…gw.example.com:443` and `…gw.example.com?x`, but not
   `…gw.example.com.evil.net` or `…gw.example.comx`. The scheme is compared the
   way the driver reads it: `grainlift://` is `https://` and
   `grainlift+<transport>://` is `<transport>://` (so a secret scoped to
   `grainlift://gw` serves `grainlift+https://gw/…`).
   **A secret without a scope (`SCOPE ''`) never matches**: it is used only
   when named.

Options given in the ATTACH clause (or a `key=value;…` path) override the same
keys from the secret. `grainlift_which_secret(uri [, attach_secret])` returns
the name of the secret an ATTACH would use (NULL for none), resolved the same
way.

Credentials stay out of `duckdb_databases()`: values that came from a secret are
never written into the attachment's options, and credential options given
inline (`bearer_token`, `token`, `password`, `oauth_refresh_token`,
`oauth_client_secret`, and any key containing `password`, `secret`,
`credential` or `api_key`, or ending in `token`) show as `<redacted>` in its
`options` and `path`. The extension writes no logs and keeps no cache keyed by
options. `duckdb_secrets()` redacts the same keys, and every `extra_options` key.

**Behaviour change (0.5.0).** Earlier versions used DuckDB's plain prefix match,
so a secret scoped to `grainlift+https://gw.example.com` was also sent to
`grainlift+https://gw.example.com.evil.net`, and a secret with an empty scope
was sent to every gateway. Neither is picked up automatically any more: give
such a secret a scope that ends at the host (or path), or name it with
`attach_secret`. `secret ''`, a no-op before, now disables lookup like
`attach_secret ''`, and naming a secret of another type is an error.

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

# Native (loads the engine's published httpfs extension for HTTP).
GEN=ninja make release

# DuckDB-WASM, COI (wasm_threads) and eh; needs emsdk 6.0.13 (Haybarn 2.0) and
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
  ./build/release/test/unittest --test-config test/configs/grainlift.json "*grainlift*"
```

`test/sql/grainlift_postgres.test` needs `GRAINLIFT_PG=1` (skipped otherwise).
Place the engine's `httpfs.duckdb_extension` beside Grainlift in
`build/release/repository/v2.0.0/<platform>/` when running the SQL suite without
building httpfs. The secret-boundary test requires no gateway or httpfs.

On 2026-11-03, the DuckDB 2.0.0 macOS ARM64 build passed all 18 SQL test
cases (1,127 assertions) against real SQLite and PostgreSQL targets. The
macOS artifact from the distribution workflow also passed
`test/python/release_smoke.py` with the released `duckdb==2.0.0` Python package.
After publication, `test/python/release_smoke.py --published` downloads the
Query.Farm extension and runs the same gateway checks with signature verification.
This check passed for the signed macOS ARM64 artifact published by the
[2.0 release workflow](https://github.com/Query-farm/duckdb-grainlift/actions/runs/41701561441).

The same validation also built both WASM flavors with Emscripten 6.0.13 and
tested them in Chromium against `@haybarn/haybarn-wasm@2.0.0-rc11`: HTTP passed
on `wasm_eh` and `wasm_threads`, and Iroh passed on `wasm_threads`, including
parameterized queries, ingestion and rollback. These used unsigned local
artifacts; signed registry publication is a separate release step. The build
pins and results are recorded in [the validation record](test/results/2026-11-03-2.0.json).

Browser: `browser-test/` is a small page on the published `@haybarn/haybarn-wasm`
bundle. `npm install && npm run build`, copy the built `.wasm` files to
`browser-test/repo/v2.0.0/<platform>/`, run `python3 serve.py 8080` (COOP/COEP
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
./build-shell.sh          # builds the shell from ~/Development/haybarn/haybarn-wasm-2.0
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

## Releasing

Pushes to `v2.0` run the native build, SQL tests, and a real SQLite gateway
smoke test before signing and publishing the DuckDB 2.0 extension. The manual
workflow's `dry_run` option signs and verifies without uploading.

For Haybarn, update `extensions/grainlift/description.yml` on the
`haybarn-2.0` branch of `Query-farm-haybarn/haybarn-community-extensions` to
the tested extension commit, then dispatch its `build.yml` with
`extension_name=grainlift`, `duckdb_version=haybarn-v2.0.0-rc11`, and
`deploy=true`. The engine submodule and browser npm dependency must match that
release exactly. Keep the registry's `variegata` reference for older engines.

After publication, run `test/python/browser_release_smoke.py` with Playwright
against the local browser page and test gateway. It installs from the Haybarn
community repository with signature verification enabled and checks both WASM
flavors. `--identity-only` reports the browser test principal for an Iroh run;
after authorizing that principal, pass `--iroh-server <endpoint-id>` to exercise
the threaded build over Iroh. The deterministic default key is for this local
test fixture only.
