"""Unit tests for the mock Lambda/Discord server (T2.3).

The mock (tests/local/discord/mock_lambda_server.py) is plain Python, so its
Discord-API surface is unit-testable without Docker: the module is imported
directly and its ``Handler`` is served from a ThreadingHTTPServer on an
ephemeral port, then exercised over real HTTP — the same transport the RIE
containers use in the integration harness.

Covered contracts:
- ``__discord_requests`` records ``{method, path, body}`` for every
  Discord-API-shaped request (any method under ``/api/v<N>/``).
- ``discord_sequences`` in ``__reset`` serves canned per-path response
  sequences (``"<METHOD> <path-suffix>"`` keys); each request consumes the
  next entry and the last entry repeats when exhausted.
- Default behavior without a sequence stays ``200 {}``.
- The pre-existing ``__discord_patches`` contract is unchanged.
- ``__reset`` clears sequences, positions, and all logs.
"""

import importlib.util
import json
import threading
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[3]
MOCK_SERVER_PATH = REPO_ROOT / "tests" / "local" / "discord" / "mock_lambda_server.py"


def _load_mock_module():
    spec = importlib.util.spec_from_file_location("mock_lambda_server", MOCK_SERVER_PATH)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture(scope="module")
def mock_server():
    """The mock served on an ephemeral 127.0.0.1 port for the whole module."""
    module = _load_mock_module()
    server = ThreadingHTTPServer(("127.0.0.1", 0), module.Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    base_url = f"http://127.0.0.1:{server.server_address[1]}"
    yield base_url
    server.shutdown()
    server.server_close()
    thread.join(timeout=5)


@pytest.fixture
def base_url(mock_server):
    """A clean mock: every test starts from a plain __reset."""
    _request("POST", f"{mock_server}/__reset", {})
    return mock_server


def _request(method, url, payload=None):
    """HTTP helper returning (status, parsed-json-or-None); 4xx/5xx don't raise."""
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(url, data=data, method=method)
    request.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            body = response.read().decode("utf-8")
            return response.status, json.loads(body) if body else None
    except urllib.error.HTTPError as error:
        body = error.read().decode("utf-8")
        return error.code, json.loads(body) if body else None


ORIGINAL_PATH = "/api/v10/webhooks/app-1/tok-1/messages/@original"
WEBHOOK_PATH = "/api/v10/webhooks/app-1/tok-1"


def test_sequence_consumed_in_order(base_url):
    _request(
        "POST",
        f"{base_url}/__reset",
        {
            "discord_sequences": {
                "PATCH /messages/@original": [
                    {"status": 429, "body": {"retry_after": 0.05}},
                    {"status": 200, "body": {"id": "111"}},
                ]
            }
        },
    )

    status, body = _request("PATCH", f"{base_url}{ORIGINAL_PATH}", {"content": "one"})
    assert status == 429
    assert body == {"retry_after": 0.05}

    status, body = _request("PATCH", f"{base_url}{ORIGINAL_PATH}", {"content": "two"})
    assert status == 200
    assert body == {"id": "111"}


def test_sequence_last_entry_repeats_when_exhausted(base_url):
    _request(
        "POST",
        f"{base_url}/__reset",
        {
            "discord_sequences": {
                "PATCH /messages/@original": [
                    {"status": 429, "body": {"retry_after": 0.05}},
                    {"status": 200, "body": {"final": True}},
                ]
            }
        },
    )

    statuses = [
        _request("PATCH", f"{base_url}{ORIGINAL_PATH}", {"content": "x"})
        for _ in range(4)
    ]
    assert [status for status, _ in statuses] == [429, 200, 200, 200]
    assert statuses[-1][1] == {"final": True}


def test_sequence_matches_method_and_path_suffix(base_url):
    _request(
        "POST",
        f"{base_url}/__reset",
        {
            "discord_sequences": {
                "POST /webhooks/app-1/tok-1": [{"status": 200, "body": {"id": "9001"}}]
            }
        },
    )

    # The sequence is keyed on POST — a PATCH to a different path stays default.
    status, body = _request("PATCH", f"{base_url}{ORIGINAL_PATH}", {"content": "x"})
    assert status == 200
    assert body == {}

    status, body = _request("POST", f"{base_url}{WEBHOOK_PATH}", {"content": "hi"})
    assert status == 200
    assert body == {"id": "9001"}


def test_default_discord_response_is_200_empty_object(base_url):
    for method, path in (
        ("PATCH", ORIGINAL_PATH),
        ("POST", WEBHOOK_PATH),
        ("DELETE", f"{WEBHOOK_PATH}/messages/9001"),
        ("GET", ORIGINAL_PATH),
    ):
        status, body = _request(method, f"{base_url}{path}", {} if method in ("PATCH", "POST") else None)
        assert status == 200, f"{method} {path}"
        assert body == {}, f"{method} {path}"


def test_discord_requests_log_records_method_path_body(base_url):
    _request("PATCH", f"{base_url}{ORIGINAL_PATH}", {"content": "patched"})
    _request("POST", f"{base_url}{WEBHOOK_PATH}", {"content": "followup"})
    _request("DELETE", f"{base_url}{WEBHOOK_PATH}/messages/9001")

    status, requests = _request("GET", f"{base_url}/__discord_requests")
    assert status == 200
    assert requests == [
        {"method": "PATCH", "path": ORIGINAL_PATH, "body": {"content": "patched"}},
        {"method": "POST", "path": WEBHOOK_PATH, "body": {"content": "followup"}},
        {"method": "DELETE", "path": f"{WEBHOOK_PATH}/messages/9001", "body": {}},
    ]


def test_discord_patches_backcompat_contract_unchanged(base_url):
    _request("PATCH", f"{base_url}{ORIGINAL_PATH}", {"content": "patched"})
    # Non-@original traffic must NOT leak into the legacy patch log.
    _request("POST", f"{base_url}{WEBHOOK_PATH}", {"content": "followup"})
    _request("DELETE", f"{base_url}{WEBHOOK_PATH}/messages/9001")

    status, patches = _request("GET", f"{base_url}/__discord_patches")
    assert status == 200
    assert patches == [
        {
            "application_id": "app-1",
            "interaction_token": "tok-1",
            "payload": {"content": "patched"},
        }
    ]


def test_reset_clears_sequences_positions_and_logs(base_url):
    _request(
        "POST",
        f"{base_url}/__reset",
        {
            "discord_sequences": {
                "PATCH /messages/@original": [
                    {"status": 429, "body": {"retry_after": 0.05}},
                    {"status": 200, "body": {}},
                ]
            }
        },
    )
    _request("PATCH", f"{base_url}{ORIGINAL_PATH}", {"content": "x"})

    _request("POST", f"{base_url}/__reset", {})

    # Sequence gone: response reverts to the 200 {} default, not the leftover
    # 200 tail of the old sequence (and certainly not another 429).
    status, body = _request("PATCH", f"{base_url}{ORIGINAL_PATH}", {"content": "y"})
    assert status == 200
    assert body == {}

    _request("POST", f"{base_url}/__reset", {})
    _, requests = _request("GET", f"{base_url}/__discord_requests")
    _, patches = _request("GET", f"{base_url}/__discord_patches")
    _, logs = _request("GET", f"{base_url}/__logs")
    assert requests == []
    assert patches == []
    assert logs == []


def test_sequence_position_restarts_after_reset(base_url):
    sequences = {
        "discord_sequences": {
            "PATCH /messages/@original": [
                {"status": 429, "body": {"retry_after": 0.05}},
                {"status": 200, "body": {}},
            ]
        }
    }
    _request("POST", f"{base_url}/__reset", sequences)
    status, _ = _request("PATCH", f"{base_url}{ORIGINAL_PATH}", {"content": "x"})
    assert status == 429

    # Re-configuring the same sequence restarts consumption from the top.
    _request("POST", f"{base_url}/__reset", sequences)
    status, _ = _request("PATCH", f"{base_url}{ORIGINAL_PATH}", {"content": "x"})
    assert status == 429
