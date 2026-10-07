// Read-only browser regression against the Cloudflare blog's shared demo.
// Start query-farm-astro in dev mode (unsigned extension loading is required).
// BLOG_PREVIEW_URL defaults to http://127.0.0.1:4325.
// GRAINLIFT_WASM optionally replaces the community extension with a local build.
// HAYBARN_DIST optionally serves the same pinned engine bytes from local disk.
// PROFILE_OUTPUT writes sanitized method/timing data; payloads are never saved.
// GRAINLIFT_TEST_ENDPOINT optionally routes RPCs to a local worker for validation.
import assert from 'node:assert/strict';
import { readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
const { chromium } = await import(process.env.PLAYWRIGHT_MODULE || 'playwright');
const browser = await chromium.launch();
const context = await browser.newContext();
if (process.env.GRAINLIFT_TEST_ENDPOINT) {
  await context.route('https://grainlift-cloudflare-public.rusty-bb6.workers.dev/**', async route => {
    const url = new URL(route.request().url());
    const response = await route.fetch({ url: `${process.env.GRAINLIFT_TEST_ENDPOINT}${url.pathname}${url.search}` });
    await route.fulfill({ response });
  });
}
const rows = [];
const measurements = [];
let phase = 'initial';
let replacementLoads = 0;
context.on('requestfinished', request => {
  if (!request.url().includes('grainlift-cloudflare-public.rusty-bb6.workers.dev')) return;
  rows.push({ phase, method: request.method(), path: new URL(request.url()).pathname, timing: request.timing() });
});
if (process.env.GRAINLIFT_WASM) {
  const body = await readFile(process.env.GRAINLIFT_WASM);
  await context.route('**/grainlift.duckdb_extension.wasm', route => {
    replacementLoads++;
    return route.fulfill({ body, contentType: 'application/wasm', headers: { 'access-control-allow-origin': '*' } });
  });
}
if (process.env.HAYBARN_DIST) {
  await context.route('https://unpkg.com/@haybarn/haybarn-wasm@*/dist/*', async route => {
    const file = path.basename(new URL(route.request().url()).pathname);
    const body = await readFile(path.join(process.env.HAYBARN_DIST, file));
    await route.fulfill({ body, contentType: file.endsWith('.wasm') ? 'application/wasm' : 'text/javascript', headers: { 'access-control-allow-origin': '*' } });
  });
}
const page = await context.newPage();
const errors = [];
page.on('pageerror', error => errors.push(error.message));
await page.addInitScript(() => {
  window.__profileTerminals = [];
  let terminal;
  Object.defineProperty(window, 'Terminal', {
    configurable: true,
    get() { return terminal; },
    set(value) {
      terminal = new Proxy(value, { construct(target, args) {
        const instance = Reflect.construct(target, args);
        window.__profileTerminals.push(instance);
        return instance;
      } });
    },
  });
});
const terminalText = () => page.evaluate(() => {
  const t = window.__profileTerminals[0];
  return t ? Array.from({length:t.buffer.active.length}, (_,i) => t.buffer.active.getLine(i)?.translateToString()).join('\n').trim() : '';
});
const join = 'SELECT p.name, sum(o.quantity) AS units, round(sum(o.quantity * p.price_cents) / 100.0, 2) AS revenue FROM shop.orders o JOIN shop.products p ON p.id = o.product_id GROUP BY p.name ORDER BY revenue DESC';
async function query(name, sql, expected) {
  phase = name;
  const before = await terminalText();
  const begin = rows.length;
  const started = performance.now();
  await page.locator('.xterm-helper-textarea').first().focus();
  await page.keyboard.insertText(sql + ';');
  await page.keyboard.press('Enter');
  await page.waitForFunction(before => {
    const t = window.__profileTerminals[0];
    const s = Array.from({length:t.buffer.active.length}, (_,i) => t.buffer.active.getLine(i)?.translateToString()).join('\n').trim();
    return s !== before && s.endsWith('H >');
  }, before, {timeout:120000});
  const output = (await terminalText()).slice(before.length - 3);
  assert(!/Error:|Exception:/.test(output), `query ${name} failed`);
  assert(output.includes(expected), `query ${name} returned unexpected rows`);
  const requests = rows.slice(begin);
  const result = { name, elapsed_ms: performance.now() - started, request_count: requests.length,
    network_ms: requests.reduce((n,r) => n+r.timing.responseEnd,0),
    methods: Object.fromEntries([...new Set(requests.map(r=>r.path))].map(p=>[p,requests.filter(r=>r.path===p).length])) };
  measurements.push(result);
  console.log(JSON.stringify(result));
  return result;
}
try {
  await page.goto(`${process.env.BLOG_PREVIEW_URL || 'http://127.0.0.1:4325'}/blog/query-cloudflare-from-duckdb/`, {waitUntil:'networkidle'});
  await page.locator('.example-try-button').first().click();
  await page.waitForFunction(() => {
    const t = window.__profileTerminals[0];
    if (!t) return false;
    const s = Array.from({length:t.buffer.active.length}, (_,i) => t.buffer.active.getLine(i)?.translateToString()).join('\n').trim();
    return s.includes('House blend') && s.endsWith('H >');
  }, {}, {timeout:180000});
  for (let n=1;n<=2;n++) {
    const result = await query(`join-warm-${n}`,join,'126.0');
    if (process.env.GRAINLIFT_WASM) {
      assert(!result.methods['/health'], 'RPC responses must supply HTTP capabilities');
      assert(!result.methods['/org.queryfarm.Grainlift.v1/get_statistics'], 'explicit unsupported statistics must be answered locally');
      assert.equal(result.methods['/org.queryfarm.Grainlift.v1/get_table_schema'], 2, 'both tables must fetch fresh schemas');
      assert(result.request_count < 25, 'warm query must use fewer requests than the baseline');
    }
  }
  // Direct scan-table calls must continue to discover schema independently.
  const direct = await query('direct-scan-table', "SELECT name FROM grainlift_scan_table('shop', 'products') ORDER BY name", 'House blend');
  assert(direct.methods['/org.queryfarm.Grainlift.v1/get_table_schema'] >= 1);
  await query('refresh-catalog','CALL grainlift_clear_cache()','H >');
  const refreshed = await query('after-refresh', join, '126.0');
  assert(refreshed.methods['/org.queryfarm.Grainlift.v1/get_table_schema'] >= 1, 'cache refresh must fetch table schemas again');
  if (process.env.GRAINLIFT_WASM) {
    assert.equal(replacementLoads,1,'local WASM was actually loaded');
    assert(!rows.some(row => row.path === '/health'), 'cold and warm connections must avoid health probes');
  }
  assert.deepEqual(errors,[]);
} finally {
  if (process.env.PROFILE_OUTPUT) await writeFile(process.env.PROFILE_OUTPUT, JSON.stringify({local_extension:!!process.env.GRAINLIFT_WASM,measurements,requests:rows},null,2)+'\n');
  await browser.close();
}
