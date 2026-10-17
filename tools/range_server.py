"""(Test tool from the launcher jobs J1-J3, used by tools/linux_headless_run.sh.) A tiny HTTP server with Range support, like the content download server: HEAD gets 404, GET honours
"Range: bytes=N-" / "bytes=N-M" with 206 + Content-Range. Serves the files of one folder.
Usage: range_server.py <folder> <port> [--throttle MBps]"""
import os
import re
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.abspath(sys.argv[1])
PORT = int(sys.argv[2])
THROTTLE = float(sys.argv[4]) * 1e6 if len(sys.argv) > 4 and sys.argv[3] == '--throttle' else 0


class Handler(BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, fmt, *args):
        sys.stderr.write('%s %s\n' % (time.strftime('%H:%M:%S'), fmt % args))

    def do_HEAD(self):
        self.send_response(404)
        self.send_header('Content-Length', '0')
        self.end_headers()

    def do_GET(self):
        name = os.path.basename(self.path.split('?')[0])
        path = os.path.join(ROOT, name)
        if not name or not os.path.isfile(path):
            self.send_response(404)
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        size = os.path.getsize(path)
        first, last, status = 0, size - 1, 200
        m = re.match(r'bytes=(\d+)-(\d*)$', self.headers.get('Range', ''))
        if m:
            first = int(m.group(1))
            last = min(int(m.group(2)), size - 1) if m.group(2) else size - 1
            if first >= size:
                self.send_response(416)
                self.send_header('Content-Range', 'bytes */%d' % size)
                self.send_header('Content-Length', '0')
                self.end_headers()
                return
            status = 206
        self.send_response(status)
        self.send_header('Content-Type', 'application/zip')
        self.send_header('Accept-Ranges', 'bytes')
        self.send_header('Content-Length', str(last - first + 1))
        if status == 206:
            self.send_header('Content-Range', 'bytes %d-%d/%d' % (first, last, size))
        self.end_headers()
        with open(path, 'rb') as f:
            f.seek(first)
            left = last - first + 1
            start = time.time()
            sent = 0
            while left > 0:
                chunk = f.read(min(left, 1 << 20))
                if not chunk:
                    break
                try:
                    self.wfile.write(chunk)
                except (ConnectionError, OSError):
                    return
                left -= len(chunk)
                sent += len(chunk)
                if THROTTLE:
                    ahead = sent / THROTTLE - (time.time() - start)
                    if ahead > 0:
                        time.sleep(ahead)


ThreadingHTTPServer(('127.0.0.1', PORT), Handler).serve_forever()
