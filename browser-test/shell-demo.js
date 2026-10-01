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

const $ = (id) => document.getElementById(id);
const setStatus = (id, state, text) => {
    const el = $(id);
    el.dataset.state = state;
    el.querySelector('.value').textContent = text;
};

const params = new URLSearchParams(location.search);
const isLocal = ['127.0.0.1', 'localhost'].includes(location.hostname);
const stored = (key) => {
    try {
        return localStorage.getItem(`grainlift-demo.${key}`) || '';
    } catch {
        return '';
    }
};
const store = (key, value) => {
    try {
        localStorage.setItem(`grainlift-demo.${key}`, value);
    } catch {
        // storage unavailable: settings last for this page load only
    }
};
// Iroh by default on the hosted demo (a browser page cannot reach a local
// HTTP service); HTTP by default when served locally.
const transport = (params.get('transport') || stored('transport') || (isLocal ? 'http' : 'iroh')) === 'iroh' ? 'iroh' : 'http';
const httpServer = params.get('server') || stored('server') || 'grainlift+http://127.0.0.1:8484';
// The browser's Iroh identity: fresh per page unless ?irohKey= pins one.
const irohKey = params.get('irohKey') || '';
// The grainlift server's Iroh EndpointId: ?irohServer=, the last one used, or
// (served locally) the test server's published record.
let irohServer = (params.get('irohServer') || stored('irohServer')).trim().toLowerCase();
if (!irohServer && transport === 'iroh' && isLocal) {
    try {
        irohServer = (await (await fetch('iroh-endpoint.json')).json()).endpoint_id;
    } catch {
        irohServer = '';
    }
}
const irohValid = /^[0-9a-f]{64}$/.test(irohServer);
const configured = transport === 'http' || irohValid;
if (irohValid) store('irohServer', irohServer);
const token = params.get('token') || 'grainlift-test-token';
const repo = `${location.origin}/repo`;
const serverUri = transport === 'iroh' ? `grainlift+iroh://${irohServer}` : httpServer;

// Service settings form.
$('cfg-iroh').value = irohServer;
$('cfg-http').value = httpServer;
$('cfg-form').dataset.transport = transport;
$('cfg-form').onsubmit = (event) => {
    event.preventDefault();
    const next = new URLSearchParams(params);
    next.set('transport', transport);
    if (transport === 'iroh') {
        next.set('irohServer', $('cfg-iroh').value.trim().toLowerCase());
    } else {
        next.set('server', $('cfg-http').value.trim());
        store('server', $('cfg-http').value.trim());
    }
    location.search = `?${next}`;
};
$('cfg-share').onclick = async () => {
    const link = new URL(location.pathname, location.origin);
    link.searchParams.set('transport', transport);
    if (transport === 'iroh') link.searchParams.set('irohServer', irohServer);
    else link.searchParams.set('server', httpServer);
    await navigator.clipboard.writeText(link.href);
    $('cfg-share').textContent = 'Copied';
    setTimeout(() => ($('cfg-share').textContent = 'Copy link'), 1500);
};
if (!configured) {
    $('cfg-hint').hidden = false;
    for (const id of ['st-pg', 'st-demo']) setStatus(id, 'error', 'no service configured');
}
const auth = transport === 'iroh' ? '' : `, BEARER_TOKEN '${token}'`;

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
    const adapter = new Worker(`iroh-adapter.js${irohKey ? `?key=${encodeURIComponent(irohKey)}` : ''}`, { type: 'module' });
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
    a.onclick = () => store('transport', a.dataset.transport);
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
];
if (configured) setup.push(
    `ATTACH '${serverUri}' AS pg (TYPE grainlift, TARGET 'postgres'${auth});`,
    `ATTACH '${serverUri}' AS demo (TYPE grainlift, TARGET 'sqlite_demo'${auth});`,
    `SELECT database, schema, name, column_names FROM (SHOW ALL TABLES) WHERE database IN ('pg', 'demo');`,
);
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
        sql: `SELECT * FROM grainlift_scan('pg', 'SELECT sensor, round(avg(reading)::numeric, 2)::float8 AS avg_reading, count(*) FILTER (WHERE NOT ok) AS faults FROM measurements WHERE taken_at < $1 GROUP BY sensor ORDER BY sensor', params := row(TIMESTAMP '2025-06-01 12:00:00'));`,
    },
    {
        title: 'Clean up',
        text: 'DDL and DML go straight to the remote with grainlift_execute.',
        sql: `SELECT * FROM grainlift_execute('pg', 'DROP TABLE IF EXISTS sales.top_customers');`,
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
