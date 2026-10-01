// Minimal haybarn-wasm console for exercising the grainlift extension in a
// browser. Exposes window.grainliftTest.sql(text) for Playwright.
import * as duckdb from '@haybarn/haybarn-wasm';
import { installVgiWebWorkerBridge } from '@haybarn/haybarn-wasm/vgi';

const params = new URLSearchParams(location.search);
const wantEh = params.get('bundle') === 'eh';
// ?transport=iroh&irohServer=<64-hex EndpointId> connects over Iroh (COI only).
const transport = params.get('transport') === 'iroh' ? 'iroh' : 'http';
const irohServer = params.get('irohServer') || '';
const serverUri =
    transport === 'iroh'
        ? `grainlift+iroh://${irohServer}`
        : params.get('server') || 'grainlift+http://127.0.0.1:8484';
const token = params.get('token') || 'grainlift-test-token';
// Iroh authenticates the browser node's key; the bearer token is HTTP only.
const attachAuth = transport === 'iroh' ? '' : `, bearer_token '${token}'`;
const repo = `${location.origin}/repo`;

const logEl = document.getElementById('log');
function log(line) {
    logEl.textContent += line + '\n';
    console.log(line);
}

const base = new URL('.', location.href).href;
const coi = !wantEh && self.crossOriginIsolated;
const bundle = coi
    ? {
          mainModule: base + 'duckdb-coi.wasm',
          mainWorker: base + 'duckdb-browser-coi.worker.js',
          pthreadWorker: base + 'duckdb-browser-coi.pthread.worker.js',
      }
    : { mainModule: base + 'duckdb-eh.wasm', mainWorker: base + 'duckdb-browser-eh.worker.js', pthreadWorker: null };

const worker = new Worker(bundle.mainWorker);

// iroh:// support (COI only): the page owns one Iroh node in an adapter Worker
// and Haybarn's bridge hands it the extension's SharedArrayBuffer rings.
let irohEndpointId = null;
if (coi) {
    const irohKey = params.get('irohKey') || '';
    const irohAdapter = new Worker(`iroh-adapter.js?key=${encodeURIComponent(irohKey)}`, { type: 'module' });
    irohEndpointId = new Promise((resolve) => {
        irohAdapter.addEventListener('message', (event) => {
            if (event.data && event.data.type === 'grainlift-iroh-node') {
                log(event.data.error ? `iroh node failed: ${event.data.error}` : `iroh node ${event.data.endpointId}`);
                resolve(event.data.endpointId || null);
            }
        });
    });
    installVgiWebWorkerBridge({ irohAdapterWorker: irohAdapter })(worker);
}
const db = new duckdb.AsyncDuckDB(new duckdb.ConsoleLogger(duckdb.LogLevel.WARNING), worker);
await db.instantiate(bundle.mainModule, bundle.pthreadWorker);
await db.open({ allowUnsignedExtensions: true });
const conn = await db.connect();
log(`engine ${await db.getVersion()} bundle=${coi ? 'coi (wasm_threads)' : 'eh'} crossOriginIsolated=${self.crossOriginIsolated}`);

function render(table) {
    const cols = table.schema.fields.map((f) => f.name);
    const rows = table.toArray().map((r) => r.toJSON());
    const t = document.createElement('table');
    t.innerHTML =
        '<tr>' + cols.map((c) => `<th>${c}</th>`).join('') + '</tr>' +
        rows.slice(0, 50).map((r) => '<tr>' + cols.map((c) => `<td>${String(r[c])}</td>`).join('') + '</tr>').join('');
    return { element: t, cols, rows };
}

async function sql(text) {
    const started = performance.now();
    try {
        const table = await conn.query(text);
        const out = render(table);
        const ms = Math.round(performance.now() - started);
        log(`ok (${out.rows.length} rows, ${ms} ms): ${text}`);
        document.getElementById('results').prepend(out.element);
        return { ok: true, cols: out.cols, rows: JSON.parse(JSON.stringify(out.rows, (k, v) => (typeof v === 'bigint' ? v.toString() : v))) };
    } catch (e) {
        log(`error: ${text}\n  ${e.message}`);
        return { ok: false, error: e.message };
    }
}

// Example queries (buttons). `demo` = sqlite_demo target, `pg` = postgres target.
const examples = {
    'SQLite: cities': `SELECT * FROM demo.cities ORDER BY population DESC`,
    'PG: customers': `SELECT * FROM pg.sales.customers ORDER BY id`,
    'PG: revenue by country': `SELECT country, orders, revenue::DECIMAL(14,2) AS revenue FROM pg.sales.revenue_by_country ORDER BY revenue DESC`,
    'PG: monthly orders (pushdown + DuckDB agg)': `SELECT date_trunc('month', ordered_at) AS month, status, count(*) AS orders, sum(amount::DECIMAL(12,2)) AS total
FROM pg.sales.orders WHERE status <> 'cancelled' GROUP BY ALL ORDER BY month, status LIMIT 24`,
    'PG: join with SQLite': `SELECT c.name AS customer, d.name AS city
FROM pg.sales.customers c JOIN demo.cities d ON d.id = c.id ORDER BY c.id`,
    'PG: grainlift_scan with $n params': `SELECT * FROM grainlift_scan('pg',
  'SELECT id, amount, ordered_at, status FROM sales.orders WHERE customer_id = $1 AND status = $2 ORDER BY id LIMIT 5', params := row(3, 'shipped'))`,
    'PG: EXPLAIN pushdown': `EXPLAIN SELECT count(*) FROM pg.sales.orders WHERE status = 'shipped' AND customer_id IN (1, 2)`,
};

// In-browser PostgreSQL checks: each query's rows (joined with '|') must equal `expect`.
const postgresChecks = [
    { name: 'remote vendor', sql: `SELECT info_value FROM grainlift_info('pg') WHERE info_name = 'vendor_name'`, expect: 'PostgreSQL' },
    { name: 'schemas and tables', sql: `SELECT schema_name || '.' || table_name FROM duckdb_tables() WHERE database_name = 'pg' ORDER BY 1`,
      expect: 'public.measurements\nsales.customers\nsales.orders\nsales.revenue_by_country' },
    { name: 'NULL handling', sql: `SELECT name FROM pg.sales.customers WHERE email IS NULL`, expect: 'Katherine Johnson' },
    { name: 'pushdown filter + aggregate', sql: `SELECT count(*)::VARCHAR, sum(amount::DECIMAL(12,2))::VARCHAR FROM pg.sales.orders WHERE status = 'shipped' AND customer_id IN (1, 2)`,
      expect: '4167|2082219.46' },
    { name: 'timestamptz filter', sql: `SELECT count(*)::VARCHAR FROM pg.sales.orders WHERE ordered_at >= TIMESTAMPTZ '2025-12-01 00:00:00+00'`, expect: '4243' },
    { name: '$n parameters', sql: `SELECT id::VARCHAR, amount FROM grainlift_scan('pg', 'SELECT id, amount FROM sales.orders WHERE customer_id = $1 AND status = $2 ORDER BY id LIMIT 2', params := row(2, 'paid'))`,
      expect: '1|79.19\n13|29.47' },
    { name: 'view', sql: `SELECT country || '=' || orders FROM pg.sales.revenue_by_country ORDER BY country`,
      expect: 'Austria=4167\nNetherlands=8333\nUnited Kingdom=16667\nUnited States=8333' },
    { name: '50k-row scan', sql: `SELECT count(*)::VARCHAR, count(weight_kg)::VARCHAR FROM pg.sales.orders`, expect: '50000|45000' },
    { name: 'setup scratch', sql: `CALL grainlift_execute('pg', 'DROP TABLE IF EXISTS sales.browser_returns')`, expect: null },
    { name: 'CTAS into sales', sql: `CREATE TABLE pg.sales.browser_returns AS SELECT range::BIGINT AS order_id, 'damaged' AS reason FROM range(1, 301)`, expect: '300' },
    { name: 'INSERT', sql: `INSERT INTO pg.sales.browser_returns SELECT 1000 + range, 'late' FROM range(20)`, expect: '20' },
    { name: 'rows in sales schema', sql: `SELECT count(*)::VARCHAR FROM grainlift_scan('pg', 'SELECT * FROM sales.browser_returns')`, expect: '320' },
    { name: 'BEGIN', sql: `BEGIN`, expect: null },
    { name: 'insert in txn', sql: `INSERT INTO pg.sales.browser_returns VALUES (99999, 'rolled back')`, expect: '1' },
    { name: 'ROLLBACK', sql: `ROLLBACK`, expect: null },
    { name: 'rollback held', sql: `SELECT count(*)::VARCHAR FROM pg.sales.browser_returns`, expect: '320' },
    { name: 'cleanup', sql: `CALL grainlift_execute('pg', 'DROP TABLE sales.browser_returns')`, expect: null },
    { name: 'refresh catalog', sql: `CALL grainlift_clear_cache()`, expect: null },
];

async function runPostgresChecks() {
    const results = [];
    for (const check of postgresChecks) {
        const r = await sql(check.sql);
        const actual = r.ok ? r.rows.map((row) => Object.values(row).map((v) => String(v)).join('|')).join('\n') : `ERROR: ${r.error}`;
        const pass = r.ok && (check.expect === null || actual === check.expect);
        results.push({ name: check.name, pass, actual, expect: check.expect });
    }
    const passed = results.filter((r) => r.pass).length;
    const box = document.getElementById('checks');
    box.innerHTML =
        `<p><b>PostgreSQL checks over ${transport}: ${passed}/${results.length} passed</b></p><table><tr><th></th><th>check</th><th>actual</th><th>expected</th></tr>` +
        results
            .map((r) => `<tr><td>${r.pass ? '✅' : '❌'}</td><td>${r.name}</td><td><pre>${r.actual}</pre></td><td><pre>${r.expect ?? '(any)'}</pre></td></tr>`)
            .join('') +
        '</table>';
    return { transport, passed, total: results.length, results };
}

window.grainliftTest = { sql, serverUri, token, repo, coi, transport, runPostgresChecks, irohEndpointId: () => irohEndpointId };

const buttons = document.getElementById('examples');
for (const [label, text] of Object.entries(examples)) {
    const b = document.createElement('button');
    b.textContent = label;
    b.onclick = () => {
        document.getElementById('query').value = text;
        sql(text);
    };
    buttons.appendChild(b);
}
document.getElementById('run').onclick = () => sql(document.getElementById('query').value);
document.getElementById('pgchecks').onclick = () => runPostgresChecks();
document.getElementById('query').value = examples['PG: customers'];

if (params.get('autoload') !== '0') {
    if (transport === 'iroh') {
        await irohEndpointId;
    }
    await sql(`SET custom_extension_repository = '${repo}'`);
    await sql(`INSTALL grainlift FROM '${repo}'`);
    await sql(`LOAD grainlift`);
    await sql(`ATTACH '${serverUri}' AS demo (TYPE grainlift, target 'sqlite_demo'${attachAuth})`);
    await sql(`ATTACH '${serverUri}' AS pg (TYPE grainlift, target 'postgres'${attachAuth})`);
}
document.getElementById('status').textContent = `ready (${transport})`;
