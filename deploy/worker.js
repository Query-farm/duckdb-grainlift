// Serves the grainlift demo site from the `grainlift-demo` R2 bucket.
//
// The only reason this Worker exists: DuckDB-WASM's threaded (COI) build and
// Iroh's SharedArrayBuffer rings need a cross-origin-isolated page, which
// requires COOP/COEP headers that an R2 custom domain cannot add by itself.

const TYPES = {
    html: 'text/html; charset=utf-8',
    js: 'text/javascript; charset=utf-8',
    mjs: 'text/javascript; charset=utf-8',
    css: 'text/css; charset=utf-8',
    json: 'application/json',
    wasm: 'application/wasm',
    png: 'image/png',
    svg: 'image/svg+xml',
};

function isolate(headers) {
    headers.set('Cross-Origin-Opener-Policy', 'same-origin');
    headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
    headers.set('Cross-Origin-Resource-Policy', 'same-origin');
    headers.set('X-Content-Type-Options', 'nosniff');
    return headers;
}

export default {
    async fetch(request, env) {
        if (request.method !== 'GET' && request.method !== 'HEAD') {
            return new Response('Method Not Allowed', { status: 405, headers: isolate(new Headers({ Allow: 'GET, HEAD' })) });
        }
        const url = new URL(request.url);
        let key = decodeURIComponent(url.pathname).replace(/^\/+/, '');
        if (key === '' || key.endsWith('/')) key += 'index.html';

        const object =
            request.method === 'HEAD'
                ? await env.SITE.head(key)
                : await env.SITE.get(key, { onlyIf: request.headers, range: request.headers });
        if (object === null) {
            return new Response('Not found', { status: 404, headers: isolate(new Headers({ 'Content-Type': 'text/plain' })) });
        }

        const headers = new Headers();
        object.writeHttpMetadata(headers);
        headers.set('ETag', object.httpEtag);
        const ext = key.split('.').pop().toLowerCase();
        if (!headers.has('Content-Type') && TYPES[ext]) headers.set('Content-Type', TYPES[ext]);
        // The page and the extension change between publishes; the large
        // engine/shell binaries are versioned by their package and can cache.
        headers.set(
            'Cache-Control',
            ext === 'html' || key.startsWith('repo/') ? 'no-cache' : 'public, max-age=3600',
        );
        isolate(headers);

        if (request.method === 'HEAD') {
            headers.set('Content-Length', String(object.size));
            return new Response(null, { headers });
        }
        if (!('body' in object)) {
            // A conditional request that did not match: the cached copy is current.
            return new Response(null, { status: 304, headers });
        }
        if (object.range && request.headers.has('Range')) {
            const { offset = 0, length = object.size - offset } = object.range;
            headers.set('Content-Range', `bytes ${offset}-${offset + length - 1}/${object.size}`);
            return new Response(object.body, { status: 206, headers });
        }
        return new Response(object.body, { headers });
    },
};
