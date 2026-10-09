#!/usr/bin/env python3
"""Minimal OpenAI-compatible LLM stub for botcore tests (stdlib only).

  stub_llm.py --portfile F --record DIR --tool NAME --args JSON   first reply: one tool call, then "done"
  stub_llm.py --portfile F --record DIR --text FILE               reply with FILE's content
  stub_llm.py --portfile F --record DIR --slow                    POST hangs 60 s (for Ctrl-C tests)

Binds 127.0.0.1 on a free port and writes it to --portfile when ready.
Records the last system prompt to DIR/system.txt, the last user message to DIR/user.txt and the
last tool result to DIR/tool.txt.
"""
import argparse, json, os, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ap = argparse.ArgumentParser()
ap.add_argument("--portfile", required=True)
ap.add_argument("--record", required=True)
ap.add_argument("--tool")
ap.add_argument("--args", default="{}")
ap.add_argument("--text")
ap.add_argument("--slow", action="store_true")
a = ap.parse_args()
os.makedirs(a.record, exist_ok=True)


def save(name, text):
    with open(os.path.join(a.record, name), "w") as f:
        f.write(text)


class H(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send(self, obj, code=200):
        b = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def do_GET(self):
        self.send({"data": [{"id": "stub"}]})

    def do_POST(self):
        req = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if a.slow:
            time.sleep(60)
            return
        msgs = req["messages"]
        save("auth.txt", self.headers.get("Authorization", ""))
        if msgs and msgs[0]["role"] == "system":
            save("system.txt", msgs[0]["content"])
        users = [m for m in msgs if m["role"] == "user"]
        if users:
            save("user.txt", users[-1]["content"])

        def reply(msg):
            self.send({"choices": [{"message": dict(role="assistant", **msg), "finish_reason": "stop"}]})

        if a.text:
            reply({"content": open(a.text).read()})
        elif a.tool and msgs[-1]["role"] != "tool":
            call = {"id": "c1", "type": "function", "function": {"name": a.tool, "arguments": a.args}}
            reply({"content": None, "tool_calls": [call]})
        else:
            if msgs[-1]["role"] == "tool":
                save("tool.txt", msgs[-1]["content"])
            reply({"content": "done"})


srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
tmp = a.portfile + ".tmp"
with open(tmp, "w") as f:
    f.write(str(srv.server_address[1]))
os.rename(tmp, a.portfile)
srv.serve_forever()
