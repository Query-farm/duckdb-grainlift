#!/usr/bin/env python3
"""Static server for the browser test: cross-origin isolated (COOP/COEP) so the
COI bundle is selected, CORP on everything (extension .wasm fetched by the
engine), and /repo/ mapped to the local extension repository."""
import http.server, os, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
REPO = os.environ.get('GRAINLIFT_REPO', os.path.join(ROOT, 'repo'))

class Handler(http.server.SimpleHTTPRequestHandler):
    def translate_path(self, path):
        clean = path.split('?', 1)[0].split('#', 1)[0]
        if clean.startswith('/repo/'):
            return os.path.join(REPO, clean[len('/repo/'):])
        return os.path.join(ROOT, 'site', clean.lstrip('/'))

    def end_headers(self):
        self.send_header('Cross-Origin-Opener-Policy', 'same-origin')
        self.send_header('Cross-Origin-Embedder-Policy', 'require-corp')
        self.send_header('Cross-Origin-Resource-Policy', 'cross-origin')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.send_header('Cache-Control', 'no-store')
        super().end_headers()

    def guess_type(self, path):
        if path.endswith('.wasm'):
            return 'application/wasm'
        if path.endswith('.js'):
            return 'text/javascript'
        return super().guess_type(path)

port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
http.server.ThreadingHTTPServer(('127.0.0.1', port), Handler).serve_forever()
