#!/usr/bin/env python3

import json
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


FUNCTION_PATH = re.compile(r"^/2015-03-31/functions/([^/]+)/invocations$")
DISCORD_PATCH_PATH = re.compile(r"^/api/v[0-9]+/webhooks/([^/]+)/([^/]+)/messages/@original$")
STATE = {"logs": [], "responses": {}, "discord_patches": []}


class Handler(BaseHTTPRequestHandler):
    def _read_json(self):
        length = int(self.headers.get("Content-Length", "0"))
        payload = self.rfile.read(length) if length else b""
        if not payload:
            return {}
        return json.loads(payload.decode("utf-8"))

    def _write_json(self, status_code, payload):
        data = json.dumps(payload).encode("utf-8")
        self.send_response(status_code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("x-amzn-RequestId", "local-test-request")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == "/__logs":
            self._write_json(200, STATE["logs"])
            return

        if self.path == "/__discord_patches":
            self._write_json(200, STATE["discord_patches"])
            return

        self._write_json(404, {"error": "not found"})

    def _handle_discord_patch(self):
        patch_match = DISCORD_PATCH_PATH.match(self.path)
        if patch_match is None:
            return False

        payload = self._read_json()
        STATE["discord_patches"].append(
            {
                "application_id": patch_match.group(1),
                "interaction_token": patch_match.group(2),
                "payload": payload,
            }
        )
        self._write_json(200, {"ok": True})
        return True

    def do_POST(self):
        if self.path == "/__reset":
            payload = self._read_json()
            STATE["logs"] = []
            STATE["responses"] = payload.get("responses", {})
            STATE["discord_patches"] = []
            self._write_json(200, {"ok": True})
            return

        if self._handle_discord_patch():
            return

        match = FUNCTION_PATH.match(self.path)
        if match is None:
            self._write_json(404, {"error": "not found"})
            return

        function_name = match.group(1)
        invocation_type = self.headers.get("X-Amz-Invocation-Type", "RequestResponse")
        payload = self._read_json()
        STATE["logs"].append(
            {
                "function_name": function_name,
                "invocation_type": invocation_type,
                "payload": payload,
            }
        )

        if function_name in STATE["responses"]:
            self._write_json(200, STATE["responses"][function_name])
            return

        if invocation_type == "Event":
            self._write_json(202, {})
            return

        self._write_json(404, {"error": f"no mock response configured for {function_name}"})

    def log_message(self, format, *args):
        return

    def do_PATCH(self):
        if self._handle_discord_patch():
            return

        self._write_json(404, {"error": "not found"})


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 19001
    # Bind all interfaces: RIE containers reach this server via the Docker
    # host-gateway address, which a 127.0.0.1 bind is invisible to on native
    # Linux (Docker Desktop's NAT masked this).
    server = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    server.serve_forever()


if __name__ == "__main__":
    main()
