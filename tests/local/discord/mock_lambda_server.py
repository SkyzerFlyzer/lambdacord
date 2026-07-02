#!/usr/bin/env python3

import json
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


FUNCTION_PATH = re.compile(r"^/2015-03-31/functions/([^/]+)/invocations$")
DISCORD_API_PATH = re.compile(r"^/api/v[0-9]+/")
DISCORD_PATCH_PATH = re.compile(r"^/api/v[0-9]+/webhooks/([^/]+)/([^/]+)/messages/@original$")
STATE = {
    "logs": [],
    "responses": {},
    "discord_patches": [],
    # Every Discord-API-shaped request (any method under /api/v<N>/), in
    # order: [{method, path, body}] (T2.3).
    "discord_requests": [],
    # Canned per-path response sequences, configured via __reset:
    #   {"<METHOD> <path-suffix>": [{"status": 429, "body": {...}}, ...]}
    # Each matching request consumes the next entry; the last entry repeats
    # when the sequence is exhausted. No match -> 200 {}.
    "discord_sequences": {},
    "discord_sequence_positions": {},
}


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

        if self.path == "/__discord_requests":
            self._write_json(200, STATE["discord_requests"])
            return

        if self._handle_discord_api("GET"):
            return

        self._write_json(404, {"error": "not found"})

    def _sequence_response(self, method):
        """Next canned (status, body) for `method self.path`, or None.

        Sequence keys are "<METHOD> <path-suffix>"; a request matches when the
        method is equal and the request path ends with the suffix. Consumption
        is per-key: each hit advances the position, and the last entry repeats
        once the sequence is exhausted.
        """
        for key, entries in STATE["discord_sequences"].items():
            configured_method, _, path_suffix = key.partition(" ")
            if configured_method != method or not path_suffix:
                continue
            if not self.path.endswith(path_suffix) or not entries:
                continue
            position = STATE["discord_sequence_positions"].get(key, 0)
            entry = entries[min(position, len(entries) - 1)]
            STATE["discord_sequence_positions"][key] = position + 1
            return entry.get("status", 200), entry.get("body", {})
        return None

    def _handle_discord_api(self, method):
        """Record + answer any Discord-API-shaped request (path /api/v<N>/...).

        Records {method, path, body} into the __discord_requests log; PATCHes
        of /messages/@original additionally keep feeding the legacy
        __discord_patches log so pre-T2.3 suites keep working unchanged.
        Responds from a configured sequence when one matches, else 200 {}.
        """
        if DISCORD_API_PATH.match(self.path) is None:
            return False

        payload = self._read_json()
        STATE["discord_requests"].append(
            {"method": method, "path": self.path, "body": payload}
        )

        patch_match = DISCORD_PATCH_PATH.match(self.path)
        if method == "PATCH" and patch_match is not None:
            STATE["discord_patches"].append(
                {
                    "application_id": patch_match.group(1),
                    "interaction_token": patch_match.group(2),
                    "payload": payload,
                }
            )

        canned = self._sequence_response(method)
        if canned is not None:
            self._write_json(canned[0], canned[1])
            return True

        self._write_json(200, {})
        return True

    def do_POST(self):
        if self.path == "/__reset":
            payload = self._read_json()
            STATE["logs"] = []
            STATE["responses"] = payload.get("responses", {})
            STATE["discord_patches"] = []
            STATE["discord_requests"] = []
            STATE["discord_sequences"] = payload.get("discord_sequences", {})
            STATE["discord_sequence_positions"] = {}
            self._write_json(200, {"ok": True})
            return

        if self._handle_discord_api("POST"):
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
        if self._handle_discord_api("PATCH"):
            return

        self._write_json(404, {"error": "not found"})

    def do_DELETE(self):
        if self._handle_discord_api("DELETE"):
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
