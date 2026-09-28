// grainlift demo: the Haybarn DuckDB-WASM shell with the grainlift extension
// loaded and PostgreSQL + SQLite grainlift targets attached, over HTTP or Iroh.
import * as duckdb from '@haybarn/haybarn-wasm';
import { installVgiWebWorkerBridge } from '@haybarn/haybarn-wasm/vgi';
import { embed } from './shell-build/src/shell.ts';
import { Terminal } from 'xterm';

// Capture the shell's xterm instance (same bundled module) to see its prompt.
let terminal = null;
const openTerminal = Terminal.prototype.open;
Terminal.prototype.open = function (element) {
    terminal = this;
    return openTerminal.call(this, element);
};

const params = new URLSearchParams(location.search);
const transport = params.get('transport') === 'iroh' ? 'iroh' : 'http';
const httpServer = params.get('server') || 'grainlift+http://127.0.0.1:8484';
// Test-only browser Iroh identity for the demo. Its EndpointId
// (17901aeedc9d6b11dbdb1c341f2650bd7d0d58c6fb9e54966e7be0499f242cb5) is what
// scripts/test-server.sh must map with GRAINLIFT_IROH_PRINCIPAL.
const DEMO_IROH_KEY = 'd0ea13d2a598d0e2f6729c8b45e5994df3269789c9cf3296e7113e0389317bb8';
const irohKey = params.get('irohKey') || DEMO_IROH_KEY;
// The server's EndpointId: ?irohServer=, else the local test server's record.
let irohServer = params.get('irohServer') || '';
if (!irohServer && params.get('transport') === 'iroh') {
    try {
        irohServer = (await (await fetch('iroh-endpoint.json')).json()).endpoint_id;
    } catch {
        irohServer = '';
    }
}
const token = params.get('token') || 'grainlift-test-token';
const repo = `${location.origin}/repo`;
const serverUri = transport === 'iroh' ? `grainlift+iroh://${irohServer}` : httpServer;
const auth = transport === 'iroh' ? '' : `, BEARER_TOKEN '${token}'`;
const connectAuth = transport === 'iroh' ? '' : `, 'bearer_token': '${token}'`;

const $ = (id) => document.getElementById(id);
const setStatus = (id, state, text) => {
    const el = $(id);
    el.dataset.state = state;
    el.querySelector('.value').textContent = text;
};

// ---------------------------------------------------------------------------
// Engine: always the cross-origin-isolated (wasm_threads) bundle.
if (!self.crossOriginIsolated) {
    $('fatal').hidden = false;
    throw new Error('page is not cross-origin isolated');
}
const base = new URL('.', location.href).href;
const worker = new Worker(base + 'duckdb-browser-coi.worker.js');

let irohReady = Promise.resolve(null);
{
    const adapter = new Worker(`iroh-adapter.js?key=${encodeURIComponent(irohKey)}`, { type: 'module' });
    irohReady = new Promise((resolve) => {
        adapter.addEventListener('message', (event) => {
            if (event.data?.type === 'grainlift-iroh-node') {
                if (event.data.error) {
                    setStatus('st-iroh', 'error', 'unavailable');
                } else {
                    setStatus('st-iroh', 'ok', event.data.endpointId.slice(0, 16) + '…');
                    $('st-iroh').title = event.data.endpointId;
                }
                resolve(event.data.endpointId || null);
            }
        });
    });
    installVgiWebWorkerBridge({ irohAdapterWorker: adapter })(worker);
}

setStatus('st-transport', 'ok', transport === 'iroh' ? 'Iroh (relay)' : 'HTTP (XHR)');
$('st-server').querySelector('.value').textContent = serverUri.length > 48 ? serverUri.slice(0, 48) + '…' : serverUri;
$('st-server').title = serverUri;
for (const a of document.querySelectorAll('[data-transport]')) {
    a.classList.toggle('active', a.dataset.transport === transport);
    const next = new URLSearchParams(params);
    next.set('transport', a.dataset.transport);
    a.href = `?${next}`;
}

async function resolveDatabase(progress) {
    const db = new duckdb.AsyncDuckDB(new duckdb.VoidLogger(), worker);
    await db.instantiate(base + 'duckdb-coi.wasm', base + 'duckdb-browser-coi.pthread.worker.js', progress);
    await db.open({ allowUnsignedExtensions: true });
    if (transport === 'iroh') {
        await irohReady;
    }
    setStatus('st-engine', 'ok', `${await db.getVersion()} · COI`);
    watchSetup(db);
    return db;
}

// Mark the attached databases as they appear (the shell runs the setup).
async function watchSetup(db) {
    const conn = await db.connect();
    for (let i = 0; i < 240; i++) {
        try {
            const rows = (await conn.query(`SELECT database_name FROM duckdb_databases() WHERE database_name IN ('pg', 'demo')`))
                .toArray()
                .map((r) => r.database_name);
            if (rows.includes('pg')) setStatus('st-pg', 'ok', 'attached as pg');
            if (rows.includes('demo')) setStatus('st-demo', 'ok', 'attached as demo');
            if (rows.length === 2) break;
        } catch {
            // extension not loaded yet
        }
        await new Promise((r) => setTimeout(r, 500));
    }
    await conn.close();
}

// ---------------------------------------------------------------------------
// Setup statements, executed visibly by the shell at startup. The shell reads
// them from `#queries=v0,<q1>,<q2>…` (its URL-sharing format).
const setup = [
    `INSTALL grainlift FROM '${repo}';`,
    `LOAD grainlift;`,
    `ATTACH '${serverUri}' AS pg (TYPE grainlift, TARGET 'postgres'${auth});`,
    `ATTACH '${serverUri}' AS demo (TYPE grainlift, TARGET 'sqlite_demo'${auth});`,
    `SET VARIABLE pgc = (SELECT grainlift_connect({'uri': '${serverUri}', 'target': 'postgres'${connectAuth}}));`,
    `SELECT database, schema, name, column_names FROM (SHOW ALL TABLES) WHERE database IN ('pg', 'demo');`,
];
// Mirrors extraswaps() in the shell: ' ' <-> '-', ';' <-> '~'.
const swap = (s) => [...s].map((c) => ({ ' ': '-', '-': ' ', ';': '~', '~': ';' })[c] ?? c).join('');
history.replaceState(null, '', location.pathname + location.search + '#queries=v0,' + setup.map((q) => encodeURIComponent(swap(q))).join(','));

// ---------------------------------------------------------------------------
// Guided queries: "Run" types the statement into the shell and presses Enter.
const tour = [
    {
        title: 'PostgreSQL, live from the browser',
        text: 'The attached database behaves like any DuckDB catalog.',
        sql: `SELECT * FROM pg.sales.customers ORDER BY id;`,
    },
    {
        title: 'Pushdown',
        text: 'Filters and projections run in PostgreSQL; DuckDB aggregates the rest.',
        sql: `SELECT status, count(*) AS orders, sum(amount::DECIMAL(12,2)) AS total FROM pg.sales.orders WHERE customer_id IN (1, 2) GROUP BY status ORDER BY total DESC;`,
    },
    {
        title: 'See what was pushed',
        text: 'The scan node lists the filters sent to the remote database.',
        sql: `EXPLAIN SELECT count(*) FROM pg.sales.orders WHERE status = 'shipped' AND customer_id IN (1, 2);`,
    },
    {
        title: 'Time series in DuckDB',
        text: '50,000 orders stream from PostgreSQL in Arrow batches.',
        sql: `SELECT date_trunc('month', ordered_at::TIMESTAMP) AS month, count(*) AS orders, round(sum(amount::DECIMAL(12,2)) / 1000, 1) AS revenue_k FROM pg.sales.orders WHERE status <> 'cancelled' GROUP BY ALL ORDER BY month;`,
    },
    {
        title: 'Join two remote databases',
        text: 'PostgreSQL and SQLite, both behind the same grainlift service.',
        sql: `SELECT c.name AS customer, c.country, d.name AS nearest_megacity FROM pg.sales.customers c JOIN demo.cities d ON d.id = c.id ORDER BY c.id;`,
    },
    {
        title: 'Views work too',
        text: 'A PostgreSQL view, computed remotely.',
        sql: `SELECT country, orders, revenue::DECIMAL(14,2) AS revenue FROM pg.sales.revenue_by_country ORDER BY revenue DESC;`,
    },
    {
        title: 'Write back',
        text: 'CREATE TABLE AS streams DuckDB results into PostgreSQL (schema sales).',
        sql: `CREATE OR REPLACE TABLE pg.sales.top_customers AS SELECT customer_id, count(*) AS orders, sum(amount::DECIMAL(12,2)) AS total FROM pg.sales.orders GROUP BY ALL ORDER BY total DESC LIMIT 3;`,
    },
    {
        title: 'Native SQL passthrough',
        text: 'grainlift_scan sends a query as-is, with $n parameters.',
        sql: `SELECT * FROM grainlift_scan(getvariable('pgc'), 'SELECT sensor, round(avg(reading)::numeric, 2)::float8 AS avg_reading, count(*) FILTER (WHERE NOT ok) AS faults FROM measurements WHERE taken_at < $1 GROUP BY sensor ORDER BY sensor', params := row(TIMESTAMP '2025-06-01 12:00:00'));`,
    },
    {
        title: 'Clean up',
        text: 'DDL and DML go straight to the remote with grainlift_execute.',
        sql: `SELECT * FROM grainlift_execute(getvariable('pgc'), 'DROP TABLE IF EXISTS sales.top_customers');`,
    },
];

const container = $('shell');
// The shell ignores input while a statement runs; wait for an empty prompt.
function shellIdle() {
    if (!terminal) return false;
    const buffer = terminal.buffer.active;
    const line = buffer.getLine(buffer.baseY + buffer.cursorY);
    return line !== undefined && line.translateToString(true).trimEnd() === 'duckdb>';
}
async function waitForIdle(timeoutMs = 120000) {
    const started = performance.now();
    // Require a stable idle prompt so the startup statements have all run.
    let idleSince = null;
    while (performance.now() - started < timeoutMs) {
        if (shellIdle()) {
            idleSince ??= performance.now();
            if (performance.now() - idleSince > 400) return;
        } else {
            idleSince = null;
        }
        await new Promise((r) => setTimeout(r, 50));
    }
}

// The shell reads keydown events (xterm custom key handler), so "type" the
// statement one key at a time, then press Enter.
let typing = Promise.resolve();
function typeIntoShell(sql, { delayMs = 6 } = {}) {
    typing = typing.then(async () => {
        await waitForIdle();
        const textarea = container.querySelector('.xterm-helper-textarea');
        if (!textarea) return;
        textarea.focus();
        const press = (key, keyCode) =>
            textarea.dispatchEvent(new KeyboardEvent('keydown', { key, keyCode, which: keyCode, bubbles: true, cancelable: true }));
        for (const ch of sql) {
            press(ch, ch.toUpperCase().charCodeAt(0));
            if (delayMs) await new Promise((r) => setTimeout(r, delayMs));
        }
        press('Enter', 13);
        await new Promise((r) => setTimeout(r, 100));
        await waitForIdle();
    });
    return typing;
}

const list = $('tour');
tour.forEach((step, i) => {
    const li = document.createElement('li');
    li.innerHTML = `
        <div class="step-head"><span class="num">${i + 1}</span><span class="title"></span>
          <button class="run" title="Type into the shell and run">Run ▸</button></div>
        <p class="text"></p>
        <pre class="sql"></pre>`;
    li.querySelector('.title').textContent = step.title;
    li.querySelector('.text').textContent = step.text;
    li.querySelector('.sql').textContent = step.sql;
    const run = li.querySelector('.run');
    run.onclick = async () => {
        for (const other of list.children) other.classList.remove('current');
        li.classList.add('current');
        run.disabled = true;
        await typeIntoShell(step.sql);
        run.disabled = false;
    };
    list.appendChild(li);
});

window.grainliftDemo = { typeIntoShell, waitForIdle, tour, serverUri, transport };

await embed({
    shellModule: base + 'shell_bg.wasm',
    container,
    resolveDatabase,
    fontFamily: '"JetBrains Mono", "SF Mono", Menlo, monospace',
    backgroundColor: '#15171c',
    defaultExtensions: [],
});
