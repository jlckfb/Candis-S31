#!/usr/bin/env python3
"""Candis-S31 sim preview server.

python3 -m http.server clone with the COOP/COEP headers Emscripten
pthreads (SharedArrayBuffer) require.
"""

import http.server
import socketserver
import sys

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
DIRECTORY = sys.argv[2] if len(sys.argv) > 2 else "."


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=DIRECTORY, **kwargs)

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        # The single-file build changes on every rebuild; never cache it.
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt, *args):
        sys.stderr.write("[preview] %s\n" % (fmt % args))


with socketserver.TCPServer(("127.0.0.1", PORT), Handler) as server:
    server.allow_reuse_address = True
    print(f"Candis-S31 LVGL preview: http://127.0.0.1:{PORT}/candis_s31_sim.html")
    server.serve_forever()
