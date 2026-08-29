#!/usr/bin/env python3
"""Candis-S31 sim preview server.

python3 -m http.server clone with the COOP/COEP headers Emscripten
pthreads (SharedArrayBuffer) require, plus a tiny screen-share ingest:

  POST /share/frame   body = data:image/png;base64,...  -> _latest.png
  POST /share/event   body = {"t":ms,"kind":"down|up|url","x":..,"y":..}
  GET  /share/latest.png   newest posted frame (what the user sees)
  GET  /share/events       JSON list of the last 64 pointer/URL events
"""

import base64
import json
import http.server
import socketserver
import sys
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
DIRECTORY = sys.argv[2] if len(sys.argv) > 2 else "."

FRAME_PATH = "_latest.png"
_events = []


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=DIRECTORY, **kwargs)

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        # The build changes on every rebuild; never cache it.
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt, *args):
        sys.stderr.write("[preview] %s\n" % (fmt % args))

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length)
        if self.path == "/share/frame":
            prefix = b"data:image/png;base64,"
            if body.startswith(prefix):
                with open(self.directory + "/" + FRAME_PATH, "wb") as f:
                    f.write(base64.b64decode(body[len(prefix):]))
        elif self.path == "/share/event":
            try:
                ev = json.loads(body)
                _events.append(ev)
                del _events[:-64]
            except ValueError:
                pass
        self.send_response(204)
        self.end_headers()

    def do_GET(self):
        if self.path == "/share/latest.png":
            self.send_response(200)
            self.send_header("Content-type", "image/png")
            self.end_headers()
            try:
                with open(self.directory + "/" + FRAME_PATH, "rb") as f:
                    self.wfile.write(f.read())
            except FileNotFoundError:
                pass
            return
        if self.path == "/share/events":
            self.send_response(200)
            self.send_header("Content-type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps(_events).encode())
            return
        super().do_GET()


class Server(socketserver.TCPServer):
    # Must be a class attribute: bind happens inside the constructor, so
    # setting it on the instance afterwards is too late (TIME_WAIT from the
    # frame stream would block rebinding for ~60 s on every restart).
    allow_reuse_address = True


with Server(("127.0.0.1", PORT), Handler) as server:
    print(f"Candis-S31 LVGL preview: http://127.0.0.1:{PORT}/candis_s31_sim.html")
    server.serve_forever()
