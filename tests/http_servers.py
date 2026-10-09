#!/usr/bin/env python3
"""Local servers for the HTTP client tests (stdlib only).

  http_servers.py PORTFILE   origin on a free port: /plain (Content-Length), /chunked, POST echo;
                             CONNECT proxy on the next line of PORTFILE (needs Basic u:"p w").
"""
import base64, json, os, select, socket, sys, threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class Origin(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path == "/chunked":
            self.send_response(200)
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for part in [b'{"a":', b'"chunked', b' ok"}']:
                self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
            self.wfile.write(b"0\r\n\r\n")
        else:
            b = b'{"plain":"ok"}'
            self.send_response(200)
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)

    def do_POST(self):
        body = self.rfile.read(int(self.headers["Content-Length"]))
        b = json.dumps({"auth": self.headers.get("Authorization"), "got": json.loads(body)}).encode()
        self.send_response(201)
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)


class Proxy(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_CONNECT(self):
        want = "Basic " + base64.b64encode(b"u:p w").decode()
        if self.headers.get("Proxy-Authorization") != want:
            self.send_response(407)
            self.end_headers()
            return
        host, port = self.path.rsplit(":", 1)
        up = socket.create_connection((host, int(port)))
        self.send_response(200)
        self.end_headers()
        c = self.connection
        while True:
            r, _, _ = select.select([c, up], [], [], 10)
            if not r:
                return
            for s in r:
                d = s.recv(65536)
                if not d:
                    return
                (up if s is c else c).sendall(d)


o = ThreadingHTTPServer(("127.0.0.1", 0), Origin)
p = ThreadingHTTPServer(("127.0.0.1", 0), Proxy)
threading.Thread(target=p.serve_forever, daemon=True).start()
tmp = sys.argv[1] + ".tmp"
with open(tmp, "w") as f:
    f.write("%d\n%d\n" % (o.server_address[1], p.server_address[1]))
os.rename(tmp, sys.argv[1])
o.serve_forever()
