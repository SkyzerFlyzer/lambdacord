"""Unit tests for the mock server's minimal DynamoDB surface (T5.3).

The integration `dedup` suite proves AD-9's completion-marker semantics end to
end; these tests pin down the mock DynamoDB contract itself without Docker.
The DynamoDB JSON protocol is a POST to "/" with the operation named in the
``X-Amz-Target: DynamoDB_20120810.<Op>`` header, routed BEFORE the Discord and
Lambda-invocation handlers.

Covered contracts:
- ``PutItem`` stores an item in the in-memory table keyed by the
  ``interaction_id`` "S" value (observable via the ``__dynamodb_table`` debug
  endpoint).
- A conditional ``PutItem`` (``ConditionExpression``
  ``attribute_not_exists(interaction_id)``) against an existing id answers
  400 with the real ``ConditionalCheckFailedException`` error shape
  (``__type`` = ``com.amazonaws.dynamodb.v20120810#ConditionalCheckFailedException``).
- An unconditional ``PutItem`` overwrites an existing item.
- ``GetItem`` answers ``{"Item": ...}`` on a hit and ``{}`` on a miss.
- ``__reset`` clears the table.
- The header-based routing does not break the existing Lambda-invocation
  endpoint (regression).
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

CONDITION_NOT_EXISTS = "attribute_not_exists(interaction_id)"
CONDITIONAL_CHECK_FAILED_TYPE = (
    "com.amazonaws.dynamodb.v20120810#ConditionalCheckFailedException"
)


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


def _request(method, url, payload=None, headers=None):
    """HTTP helper returning (status, parsed-json-or-None); 4xx/5xx don't raise."""
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(url, data=data, method=method)
    request.add_header("Content-Type", "application/x-amz-json-1.0")
    for key, value in (headers or {}).items():
        request.add_header(key, value)
    try:
        with urllib.request.urlopen(request, timeout=10) as response:
            body = response.read().decode("utf-8")
            return response.status, json.loads(body) if body else None
    except urllib.error.HTTPError as error:
        body = error.read().decode("utf-8")
        return error.code, json.loads(body) if body else None


def _item(interaction_id, expires_at):
    return {
        "interaction_id": {"S": interaction_id},
        "expires_at": {"N": str(expires_at)},
    }


def _put_item(base_url, interaction_id, expires_at, condition=None):
    payload = {"TableName": "test-idempotency", "Item": _item(interaction_id, expires_at)}
    if condition is not None:
        payload["ConditionExpression"] = condition
    return _request(
        "POST",
        f"{base_url}/",
        payload,
        headers={"X-Amz-Target": "DynamoDB_20120810.PutItem"},
    )


def _get_item(base_url, interaction_id):
    payload = {
        "TableName": "test-idempotency",
        "Key": {"interaction_id": {"S": interaction_id}},
        "ConsistentRead": True,
    }
    return _request(
        "POST",
        f"{base_url}/",
        payload,
        headers={"X-Amz-Target": "DynamoDB_20120810.GetItem"},
    )


def _table(base_url):
    return _request("GET", f"{base_url}/__dynamodb_table")


def test_conditional_put_stores_item(base_url):
    status, body = _put_item(base_url, "int-1", 1710003600, condition=CONDITION_NOT_EXISTS)
    assert status == 200
    assert body == {}

    status, table = _table(base_url)
    assert status == 200
    assert table == {"int-1": _item("int-1", 1710003600)}


def test_second_conditional_put_fails_with_conditional_check_failed_shape(base_url):
    status, _ = _put_item(base_url, "int-1", 1710003600, condition=CONDITION_NOT_EXISTS)
    assert status == 200

    status, body = _put_item(base_url, "int-1", 1710009999, condition=CONDITION_NOT_EXISTS)
    assert status == 400
    assert body == {
        "__type": CONDITIONAL_CHECK_FAILED_TYPE,
        "message": "The conditional request failed",
    }

    # The losing conditional write must not have touched the stored item.
    _, table = _table(base_url)
    assert table == {"int-1": _item("int-1", 1710003600)}


def test_unconditional_put_overwrites(base_url):
    status, _ = _put_item(base_url, "int-1", 1710003600, condition=CONDITION_NOT_EXISTS)
    assert status == 200

    status, body = _put_item(base_url, "int-1", 1710009999)
    assert status == 200
    assert body == {}

    _, table = _table(base_url)
    assert table == {"int-1": _item("int-1", 1710009999)}


def test_get_item_hit_returns_item(base_url):
    _put_item(base_url, "int-1", 1710003600)

    status, body = _get_item(base_url, "int-1")
    assert status == 200
    assert body == {"Item": _item("int-1", 1710003600)}


def test_get_item_miss_returns_empty_object(base_url):
    status, body = _get_item(base_url, "never-stored")
    assert status == 200
    assert body == {}


def test_reset_clears_dynamodb_table(base_url):
    _put_item(base_url, "int-1", 1710003600)
    _request("POST", f"{base_url}/__reset", {})

    status, table = _table(base_url)
    assert status == 200
    assert table == {}

    status, body = _get_item(base_url, "int-1")
    assert status == 200
    assert body == {}


def test_dynamodb_routing_does_not_break_function_invocations(base_url):
    """Regression: X-Amz-Target routing must leave the Lambda endpoint intact."""
    status, body = _request(
        "POST",
        f"{base_url}/2015-03-31/functions/discord-cmd-test-dedup/invocations",
        {"type": 2, "id": "int-1"},
        headers={"X-Amz-Invocation-Type": "Event"},
    )
    assert status == 202
    assert body == {}

    status, logs = _request("GET", f"{base_url}/__logs")
    assert status == 200
    assert logs == [
        {
            "function_name": "discord-cmd-test-dedup",
            "invocation_type": "Event",
            "payload": {"type": 2, "id": "int-1"},
        }
    ]
