#!/usr/bin/env python3
"""Fake DeepSeek API for the headless proJV end-to-end harness (development only).

Run:  py -3 fake_server.py <port> <captures.jsonl>
Behaviour:
  * POST /v1/chat/completions  -> first call answers with a view_image tool call,
                                 every later call answers with plain text.
                                 Each request body is appended to the captures file.
  * GET  /v1/models            -> two model ids.
"""
import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1])
CAPTURES = sys.argv[2]
lock = threading.Lock()
state = {"posts": 0}


def sse(obj):
    return ("data: " + json.dumps(obj) + "\n\n").encode("utf-8")


def tool_call_chunk():
    args = json.dumps({"paths": ["shot.png"]})
    return sse(
        {
            "id": "chatcmpl-e2e",
            "object": "chat.completion.chunk",
            "model": "deepseek-v4-flash",
            "choices": [
                {
                    "index": 0,
                    "delta": {
                        "tool_calls": [
                            {
                                "index": 0,
                                "id": "call_e2e_1",
                                "type": "function",
                                "function": {"name": "view_image", "arguments": args},
                            }
                        ]
                    },
                    "finish_reason": None,
                }
            ],
        }
    )


def text_chunk(text):
    return sse(
        {
            "id": "chatcmpl-e2e",
            "object": "chat.completion.chunk",
            "model": "deepseek-v4-flash",
            "choices": [
                {"index": 0, "delta": {"content": text}, "finish_reason": None}
            ],
        }
    )


def finish_chunk(reason):
    return sse(
        {
            "id": "chatcmpl-e2e",
            "object": "chat.completion.chunk",
            "model": "deepseek-v4-flash",
            "choices": [{"index": 0, "delta": {}, "finish_reason": reason}],
        }
    )


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # keep the console quiet
        pass

    def _stream(self, chunks):
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()
        for c in chunks:
            self.wfile.write(c)
            self.wfile.flush()
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()
        self.close_connection = True

    def do_POST(self):
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8", "replace")
        with lock:
            state["posts"] += 1
            n = state["posts"]
            with open(CAPTURES, "a", encoding="utf-8") as fh:
                fh.write(json.dumps({"n": n, "path": self.path, "body": body}) + "\n")

        if n == 1:
            self._stream(
                [tool_call_chunk(), finish_chunk("tool_calls")]
            )
        else:
            self._stream(
                [
                    text_chunk("the image is a 1x1 test PNG; nothing else in it."),
                    finish_chunk("stop"),
                ]
            )

    def do_GET(self):
        if self.path.startswith("/v1/models"):
            payload = {
                "object": "list",
                "data": [
                    {"id": "deepseek-v4-flash", "object": "model"},
                    {"id": "deepseek-v4-pro", "object": "model"},
                ],
            }
            raw = json.dumps(payload).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)
            return
        self.send_response(404)
        self.send_header("Content-Length", "0")
        self.end_headers()


if __name__ == "__main__":
    open(CAPTURES, "w").close()
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    print("fake DeepSeek API on 127.0.0.1:%d" % PORT, flush=True)
    srv.serve_forever()
