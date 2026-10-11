"""Tiny local echo server for the Clausewitz REST spike (stdlib only).
Run: python dev/tools/echo_server.py   (listens on 127.0.0.1:8787)
Replies JSON {"echo": <body or query/path>, "method": ..., "path": ..., "headers": {...}}.
"""
import json
from http.server import BaseHTTPRequestHandler, HTTPServer

PORT = 8787


class Handler(BaseHTTPRequestHandler):
    def _handle(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length).decode("utf-8", "replace") if length else ""
        headers = {k: v for k, v in self.headers.items()}
        print("---- %s %s" % (self.command, self.path), flush=True)
        for k, v in headers.items():
            print("  %s: %s" % (k, v), flush=True)
        print("  body: %r" % body, flush=True)
        try:
            echo = json.loads(body) if body else self.path
        except ValueError:
            echo = body
        out = json.dumps({"echo": echo, "method": self.command, "path": self.path,
                          "headers": headers}).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)

    do_GET = do_POST = do_PUT = do_DELETE = _handle

    def log_message(self, fmt, *args):
        pass


if __name__ == "__main__":
    print("echo server on http://127.0.0.1:%d" % PORT, flush=True)
    HTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
