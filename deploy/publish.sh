#!/usr/bin/env bash
# Publish the grainlift shell demo to https://grainlift-demo.query-farm.services
#
#   deploy/publish.sh            # assemble, upload to R2, deploy the Worker
#   deploy/publish.sh --no-worker  # upload site files only
#
# Prereqs: `wrangler` logged in; browser-test built (`npm run build` after
# ./build-shell.sh); the COI extension built (scripts/build-wasm.sh wasm_threads).
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$(cd .. && pwd)"
BUCKET=grainlift-demo
ENGINE_REV="${ENGINE_REV:-v1.5.5}"
SITE="$ROOT/browser-test/site"
DIST="$ROOT/deploy/dist"

rm -rf "$DIST"
mkdir -p "$DIST/repo/$ENGINE_REV/wasm_threads"
cp "$SITE/shell.html" "$DIST/index.html"
cp "$SITE/shell.html" "$DIST/shell.html"
cp "$SITE"/{shell-demo.bundle.js,shell_bg.wasm,xterm.css,iroh-adapter.js} "$DIST/"
cp "$SITE"/{duckdb-coi.wasm,duckdb-browser-coi.worker.js,duckdb-browser-coi.pthread.worker.js} "$DIST/"
cp -R "$SITE/iroh" "$DIST/iroh"
find "$DIST/iroh" -name "*.d.ts" -delete
cp "$ROOT/build/wasm_threads/extension/grainlift/grainlift.duckdb_extension.wasm" \
   "$DIST/repo/$ENGINE_REV/wasm_threads/"

type_for() {
    case "$1" in
        *.html) echo 'text/html; charset=utf-8' ;;
        *.js) echo 'text/javascript; charset=utf-8' ;;
        *.css) echo 'text/css; charset=utf-8' ;;
        *.json) echo 'application/json' ;;
        *.wasm) echo 'application/wasm' ;;
        *.ts) echo 'text/plain; charset=utf-8' ;;
        *) echo 'application/octet-stream' ;;
    esac
}

wrangler r2 bucket list 2>/dev/null | grep -q "name:\s*$BUCKET$" || wrangler r2 bucket create "$BUCKET"

(cd "$DIST" && find . -type f | sed 's|^\./||' | sort) | while read -r key; do
    echo "upload $key"
    wrangler r2 object put "$BUCKET/$key" --remote --file "$DIST/$key" --content-type "$(type_for "$key")" >/dev/null
done

if [[ "${1:-}" != "--no-worker" ]]; then
    wrangler deploy
fi
echo "published: https://grainlift-demo.query-farm.services/"
