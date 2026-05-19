#!/usr/bin/env python3

import argparse
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--event-file", required=True)
    parser.add_argument("--response-file", required=True)
    return parser.parse_args()


class RuntimeState:
    def __init__(self, event_payload, response_file):
        self.event_payload = event_payload
        self.response_file = Path(response_file)
        self.request_id = "codex-static-check-request"
        self.event_served = False
        self.response_seen = False


def make_handler(state):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, format, *args):
            return

        def do_GET(self):
            if self.path != "/2018-06-01/runtime/invocation/next" or state.event_served:
                self.send_error(404)
                return

            body = state.event_payload.encode("utf-8")
            state.event_served = True
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Lambda-Runtime-Aws-Request-Id", state.request_id)
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self):
            expected_prefix = f"/2018-06-01/runtime/invocation/{state.request_id}/"
            if not self.path.startswith(expected_prefix):
                self.send_error(404)
                return

            content_length = int(self.headers.get("Content-Length", "0"))
            payload = self.rfile.read(content_length)
            state.response_file.write_bytes(payload)
            state.response_seen = True
            self.send_response(202)
            self.end_headers()
            threading.Thread(target=self.server.shutdown, daemon=True).start()

    return Handler


def main():
    args = parse_args()
    event_payload = json.dumps(json.loads(Path(args.event_file).read_text()))
    state = RuntimeState(event_payload, args.response_file)
    state.response_file.parent.mkdir(parents=True, exist_ok=True)
    server = ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(state))

    try:
        server.serve_forever()
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
