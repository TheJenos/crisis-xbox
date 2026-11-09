"""Local server for the browser build. Serves on 127.0.0.1 only.

The page caches crysis2.wasm (checked against its ETag) and, through its
service worker (sw.js), the disc data it reads, so later visits load faster.

usage: python -I serve.py <extracted disc folder | disc image .iso> [port]

- Serves recomp/web/ (index.html, crysis2.js, crysis2.wasm).
- An extracted disc folder (made with: python -I analysis/xiso.py <iso>
  extractall extracted/disc) is served under /disc/: the game fetches each
  file only when it opens it, and only the parts it reads (HTTP ranges).
- A disc image is served as /disc.iso instead (read in pieces, never copied).
- Sends the cross-origin isolation headers browsers require for
  SharedArrayBuffer, which WebAssembly threads need.
"""
import http.server
import os
import re
import sys
import urllib.parse

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "web")
DISC = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else None
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
# Debugging only: with DUMP_DIR=<folder> set, the page may upload frame dumps
# (PUT /upload/<name>) into that folder. Unset (the default), uploads are refused.
DUMP_DIR = os.path.abspath(os.environ["DUMP_DIR"]) if os.environ.get("DUMP_DIR") else None
TYPES = {".html": "text/html", ".js": "text/javascript", ".wasm": "application/wasm", ".css": "text/css",
         ".json": "application/json", ".txt": "text/plain"}


def inside(base, path):
    full = os.path.normpath(os.path.join(base, path))
    return full if full == base or full.startswith(base + os.sep) else None


class Handler(http.server.BaseHTTPRequestHandler):
    # Keep-alive: the game makes thousands of small range requests from many
    # worker threads; a new connection for each can exhaust the backlog/ports.
    protocol_version = "HTTP/1.1"

    def isolation_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", "no-cache")

    def resolve(self):
        path = urllib.parse.unquote(self.path.split("?", 1)[0])
        if os.path.isdir(DISC) and path.startswith("/disc/"):
            return inside(DISC, path[len("/disc/"):]), "application/octet-stream"
        if os.path.isfile(DISC) and path == "/disc.iso":
            return DISC, "application/octet-stream"
        if path == "/":
            path = "/index.html"
        full = inside(ROOT, path.lstrip("/"))
        return full, TYPES.get(os.path.splitext(full or "")[1], "application/octet-stream")

    def send_file(self, head_only):
        full, ctype = self.resolve()
        if not full or not os.path.isfile(full):
            self.send_error(404)
            return
        st = os.stat(full)
        size = st.st_size
        start, end = 0, size - 1
        m = re.match(r"bytes=(\d+)-(\d*)", self.headers.get("Range", ""))
        if m:
            start = int(m.group(1))
            end = min(int(m.group(2)) if m.group(2) else size - 1, size - 1)
            if start > end:
                self.send_error(416)
                return
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        else:
            self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Accept-Ranges", "bytes")
        # Version tag: the page keeps crysis2.wasm in the browser's cache and
        # downloads it again only when this changes (a rebuild).
        self.send_header("ETag", f'"{size:x}-{st.st_mtime_ns:x}"')
        self.isolation_headers()
        self.end_headers()
        if head_only:
            return
        with open(full, "rb") as f:
            f.seek(start)
            remaining = end - start + 1
            while remaining > 0:
                chunk = f.read(min(remaining, 1 << 20))
                if not chunk:
                    break
                self.wfile.write(chunk)
                remaining -= len(chunk)

    def do_GET(self):
        self.send_file(head_only=False)

    def do_HEAD(self):
        self.send_file(head_only=True)

    def do_PUT(self):
        # Frame dumps from the page (Module._crysis_dump_frame()). Only when the
        # server was started with DUMP_DIR set; otherwise uploads are refused.
        path = urllib.parse.unquote(self.path.split("?", 1)[0])
        name = os.path.basename(path[len("/upload/"):]) if path.startswith("/upload/") else ""
        if not DUMP_DIR or not name or not re.fullmatch(r"[\w.\-]+", name):
            self.send_error(403)
            return
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0 or length > 64 << 20:
            self.send_error(413)
            return
        data = self.rfile.read(length)
        os.makedirs(DUMP_DIR, exist_ok=True)
        with open(os.path.join(DUMP_DIR, name), "wb") as f:
            f.write(data)
        self.send_response(204)
        self.isolation_headers()
        self.end_headers()

    def log_message(self, fmt, *args):
        if not getattr(self, "path", "").startswith(("/disc/", "/disc.iso")):  # disc reads are too frequent to log
            sys.stderr.write("%s\n" % (fmt % args))

    def log_error(self, fmt, *args):  # errors are always shown
        sys.stderr.write("error: %s (%s)\n" % (fmt % args, getattr(self, "path", "?")))


class Server(http.server.ThreadingHTTPServer):
    request_queue_size = 128  # many concurrent requests from the game's threads
    daemon_threads = True


if __name__ == "__main__":
    if not DISC or not os.path.exists(DISC):
        sys.exit("usage: python -I serve.py <extracted disc folder | disc image .iso> [port]")
    if os.path.isdir(DISC) and not os.path.isfile(os.path.join(DISC, "disc-index.txt")):
        sys.exit(f"{DISC} has no disc-index.txt; extract with: python -I analysis/xiso.py <iso> extractall <folder>")
    kind = "extracted disc at /disc/" if os.path.isdir(DISC) else "disc image as /disc.iso"
    print(f"serving {ROOT} and {DISC} ({kind}) on http://127.0.0.1:{PORT}/")
    Server(("127.0.0.1", PORT), Handler).serve_forever()
