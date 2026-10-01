#!/usr/bin/env bash
# Run a local grainlift-server for the extension's tests.
#
#   scripts/test-server.sh [port] [cors-origin]
#
# Serves an `sqlite` target (driver installed via `dbc install sqlite`) whose
# database URI the client may choose (`remote_uri`), plus `sqlite_demo`, a
# fixed file database seeded with sample data for the browser demo. Exports for
# the sqllogictests:
#   GRAINLIFT_URI=grainlift+http://127.0.0.1:<port>  GRAINLIFT_TOKEN=grainlift-test-token
#
# PostgreSQL (default on when initdb is available; GRAINLIFT_PG=0 disables):
#   starts a throwaway cluster in $WORK_DIR/pg on GRAINLIFT_PG_PORT (55432),
#   seeds it (scripts/pg_seed.sql) and serves it as the `postgres` target
#   (driver installed via `dbc install postgresql`). It is stopped on exit.
#
# Optional Iroh listener (for iroh:// from the browser COI build):
#   GRAINLIFT_IROH=1 enables it; the server's EndpointId is written to
#   $WORK_DIR/iroh-endpoint.json.
#   GRAINLIFT_IROH_PRINCIPAL=<client 64-hex EndpointId> maps that client to the
#   test principal (implies GRAINLIFT_IROH=1).
#
# GRAINLIFT_OPEN=1: no authentication at all — any HTTP or Iroh client may use
# every target (for a private demo link; never expose real data this way).
#   GRAINLIFT_IROH_SECRET_KEY=<64-hex> pins the server identity (default: generated).
set -euo pipefail

PORT="${1:-8484}"
CORS_ORIGIN="${2:-http://127.0.0.1:8080}"
GRAINLIFT_SOURCE_DIR="${GRAINLIFT_SOURCE_DIR:-$(cd "$(dirname "$0")/../grainlift" 2>/dev/null && pwd || true)}"
WORK_DIR="${GRAINLIFT_TEST_DIR:-${TMPDIR:-/tmp}/grainlift-test-server}"
mkdir -p "$WORK_DIR"

SERVER_BIN="${GRAINLIFT_SERVER_BIN:-}"
if [[ -z "$SERVER_BIN" ]]; then
    if [[ -z "$GRAINLIFT_SOURCE_DIR" || ! -f "$GRAINLIFT_SOURCE_DIR/Cargo.toml" ]]; then
        echo "set GRAINLIFT_SOURCE_DIR or GRAINLIFT_SERVER_BIN" >&2
        exit 1
    fi
    cargo build --release --manifest-path "$GRAINLIFT_SOURCE_DIR/Cargo.toml" -p grainlift-server >&2
    SERVER_BIN="$(cargo metadata --format-version 1 --no-deps --manifest-path "$GRAINLIFT_SOURCE_DIR/Cargo.toml" \
        | python3 -c 'import json,sys; print(json.load(sys.stdin)["target_directory"])')/release/grainlift-server"
fi

DEMO_DB="$WORK_DIR/demo.sqlite"
rm -f "$DEMO_DB"
python3 - "$DEMO_DB" <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
db.executescript("""
CREATE TABLE cities (id INTEGER PRIMARY KEY, name TEXT, country TEXT, population INTEGER, lat REAL, lon REAL);
INSERT INTO cities VALUES
  (1, 'Tokyo', 'Japan', 37400068, 35.6897, 139.6922),
  (2, 'Delhi', 'India', 28514000, 28.6600, 77.2300),
  (3, 'Shanghai', 'China', 25582000, 31.2286, 121.4747),
  (4, 'São Paulo', 'Brazil', 21650000, -23.5504, -46.6339),
  (5, 'Mexico City', 'Mexico', 21581000, 19.4333, -99.1333),
  (6, 'Cairo', 'Egypt', 20076000, 30.0444, 31.2358),
  (7, 'New York', 'United States', 18819000, 40.6943, -73.9249),
  (8, 'Lagos', 'Nigeria', 13463000, 6.4550, 3.3841);
CREATE TABLE numbers AS WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 100000) SELECT i, i * i AS sq, 'row ' || i AS label FROM n;
""")
db.commit()
PY

PG_BIN="${GRAINLIFT_PG_BIN:-$(dirname "$(command -v initdb || echo /opt/homebrew/opt/postgresql@14/bin/initdb)")}"
PG_PORT="${GRAINLIFT_PG_PORT:-55432}"
PG_ENABLED=0
if [[ "${GRAINLIFT_PG:-1}" != "0" && -x "$PG_BIN/initdb" ]]; then
    PG_ENABLED=1
    PG_DATA="$WORK_DIR/pg"
    "$PG_BIN/pg_ctl" -D "$PG_DATA" stop -m fast >/dev/null 2>&1 || true
    rm -rf "$PG_DATA"
    "$PG_BIN/initdb" -D "$PG_DATA" -U postgres --auth=trust --encoding=UTF8 --no-locale >/dev/null
    "$PG_BIN/pg_ctl" -D "$PG_DATA" -l "$WORK_DIR/pg.log" -w \
        -o "-p ${PG_PORT} -c listen_addresses=127.0.0.1 -k ${PG_DATA}" start >/dev/null
    # Stop only the postmaster this instance started: a restarted script may
    # already own a new cluster in the same data directory.
    PG_PID="$(head -1 "$PG_DATA/postmaster.pid")"
    trap '[[ "$(head -1 "$PG_DATA/postmaster.pid" 2>/dev/null)" == "$PG_PID" ]] && "$PG_BIN/pg_ctl" -D "$PG_DATA" stop -m fast >/dev/null 2>&1 || true' EXIT
    "$PG_BIN/createdb" -h 127.0.0.1 -p "$PG_PORT" -U postgres grainlift
    "$PG_BIN/psql" -q -v ON_ERROR_STOP=1 -h 127.0.0.1 -p "$PG_PORT" -U postgres -d grainlift \
        -f "$(dirname "$0")/pg_seed.sql" >/dev/null
    echo "postgres on 127.0.0.1:${PG_PORT} (database grainlift)" >&2
fi

OPEN="${GRAINLIFT_OPEN:-0}"
REQUIRE_AUTH=true
[[ "$OPEN" == "1" ]] && REQUIRE_AUTH=false

CONFIG="$WORK_DIR/grainlift.toml"
cat > "$CONFIG" <<TOML
[server]
listen = "127.0.0.1:${PORT}"
require_authentication = ${REQUIRE_AUTH}
cors_origins = "${CORS_ORIGIN}"
# Browser pages that are reloaded never close their sessions; keep the quota
# generous and let abandoned sessions expire quickly.
session_ttl_seconds = 3600
max_sessions = 4096
max_sessions_per_principal = 1024

[auth.static_bearer_tokens]
grainlift-test-token = "tester"
TOML

if [[ "$OPEN" != "1" ]]; then
    TESTER_TARGETS='"sqlite", "sqlite_demo"'
    [[ "$PG_ENABLED" == "1" ]] && TESTER_TARGETS+=', "postgres"'
    cat >> "$CONFIG" <<TOML

[auth.target_permissions]
"tester" = [${TESTER_TARGETS}]
TOML
fi

cat >> "$CONFIG" <<TOML

[targets.sqlite]
driver = "sqlite"
entrypoint = "AdbcDriverSqliteInit"
allow_client_database_options = false
allowed_client_database_options = ["uri"]
allow_client_connection_options = true

[targets.sqlite_demo]
driver = "sqlite"
entrypoint = "AdbcDriverSqliteInit"
allow_client_connection_options = true

[[targets.sqlite_demo.database_options]]
key = "uri"
type = "string"
value = "${DEMO_DB}"
TOML

if [[ "$PG_ENABLED" == "1" ]]; then
    cat >> "$CONFIG" <<TOML

[targets.postgres]
driver = "postgresql"
entrypoint = "AdbcDriverPostgresqlInit"
allow_client_connection_options = true

[[targets.postgres.database_options]]
key = "uri"
type = "string"
value = "postgresql://postgres@127.0.0.1:${PG_PORT}/grainlift"
TOML
fi

if [[ -n "${GRAINLIFT_IROH_PRINCIPAL:-}" || "${GRAINLIFT_IROH:-0}" == "1" ]]; then
    KEY_FILE="$WORK_DIR/iroh.key"
    if [[ -n "${GRAINLIFT_IROH_SECRET_KEY:-}" ]]; then
        printf '%s' "$GRAINLIFT_IROH_SECRET_KEY" > "$KEY_FILE"
    elif [[ ! -f "$KEY_FILE" ]]; then
        python3 -c 'import secrets; print(secrets.token_hex(32), end="")' > "$KEY_FILE"
    fi
    chmod 600 "$KEY_FILE"
    cat >> "$CONFIG" <<TOML

[iroh]
issuer = "grainlift-test"
secret_key_file = "${KEY_FILE}"
endpoint_info_file = "${WORK_DIR}/iroh-endpoint.json"
TOML
    if [[ -n "${GRAINLIFT_IROH_PRINCIPAL:-}" ]]; then
        cat >> "$CONFIG" <<TOML

[iroh.principals]
"${GRAINLIFT_IROH_PRINCIPAL}" = "tester"
TOML
    fi
fi

echo "grainlift-server on http://127.0.0.1:${PORT} (CORS origin ${CORS_ORIGIN}); config ${CONFIG}" >&2
# Not exec: the EXIT trap stops the PostgreSQL cluster when the server exits.
"$SERVER_BIN" --config "$CONFIG"
