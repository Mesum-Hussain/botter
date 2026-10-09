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
ap.add_argument("--nostream", action="store_true", help='answer 400 to "stream": true')
ap.add_argument("--think", action="store_true", help="start the text answer with a <think> block")
ap.add_argument("--sqnc", choices=["yes", "no"], help="answer Sqnc interpreter requests (decisions = this word)")
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

        stream = bool(req.get("stream"))
        save("stream.txt", "stream" if stream else "plain")
        if stream and a.nostream:
            self.send({"error": {"message": "stream not supported"}}, 400)
            return
        for m in msgs:
            if m.get("tool_calls"):
                save("calls.txt", json.dumps(m["tool_calls"]))

        def sse(obj):
            self.wfile.write(b"data: " + json.dumps(obj).encode() + b"\n\n")
            self.wfile.flush()
            time.sleep(0.02)

        def reply(msg):
            if not stream:
                self.send({"choices": [{"message": dict(role="assistant", **msg), "finish_reason": "stop"}]})
                return
            # Server-Sent Events: text in 3-character pieces, tool call arguments split in two
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            self.wfile.write(b": keep-alive comment\n\n")
            text = msg.get("content") or ""
            for i in range(0, len(text), 3):
                sse({"choices": [{"index": 0, "delta": {"content": text[i:i + 3]}}]})
            for k, c in enumerate(msg.get("tool_calls") or []):
                args = c["function"]["arguments"]
                h = len(args) // 2
                first = {"index": k, "id": c["id"], "type": "function",
                         "function": {"name": c["function"]["name"], "arguments": args[:h]},
                         "extra_content": {"google": {"thought_signature": "sig-%d" % k}}}
                sse({"choices": [{"index": 0, "delta": {"tool_calls": [first]}}]})
                sse({"choices": [{"index": 0, "delta": {"tool_calls": [{"index": k, "function": {"arguments": args[h:]}}]}}]})
            sse({"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]})
            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()

        if msgs and msgs[0]["role"] == "system" and msgs[0]["content"].startswith("You review SQNC.md"):
            save("review.txt", msgs[-1]["content"])
            reply({"content": json.dumps({"makes_sense": False, "summary": "one step cannot work",
                                          "problems": [{"line": 7, "statement": "EXECUTE tool `fs_read`",
                                                        "why": "reads a file that is never written",
                                                        "suggestion": "write the file first"}]})})
        elif a.sqnc and users and users[-1]["content"].startswith("[Sqnc") and msgs[-1]["role"] == "user":
            last = users[-1]["content"]
            with open(os.path.join(a.record, "sqnc.txt"), "a") as f:
                f.write(last.split("\n")[0] + "\n")
            if last.startswith("[Sqnc] Decide"):
                reply({"content": a.sqnc})
            elif last.startswith("[Sqnc] Work out"):
                reply({"content": '"worked-out value"'})
            elif last.startswith("[Sqnc] Build the JSON"):
                reply({"content": "{}"})
            elif last.startswith("[Sqnc final step"):
                reply({"content": "All done, final message."})
            else:
                reply({"content": "did: " + last.split("\n")[0][:80]})
        elif a.text:
            reply({"content": open(a.text).read()})
        elif a.tool and msgs[-1]["role"] != "tool":
            call = {"id": "c1", "type": "function", "function": {"name": a.tool, "arguments": a.args}}
            reply({"content": None, "tool_calls": [call]})
        else:
            if msgs[-1]["role"] == "tool":
                save("tool.txt", msgs[-1]["content"])
            reply({"content": "<think>pondering the task</think>Hello there, streamed reply." if a.think else "done"})


srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
tmp = a.portfile + ".tmp"
with open(tmp, "w") as f:
    f.write(str(srv.server_address[1]))
os.rename(tmp, a.portfile)
srv.serve_forever()
