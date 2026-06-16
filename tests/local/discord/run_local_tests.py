#!/usr/bin/env python3

import argparse
import base64
import json
import os
import shutil
import shlex
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO_ROOT / "scripts" / "lib"))
from discord_modules import route_map, validate_module_manifests  # noqa: E402

PACKAGED_DIR = REPO_ROOT / "packaged-lambdas"
MOCK_SERVER = REPO_ROOT / "tests" / "local" / "discord" / "mock_lambda_server.py"
MOCK_PORT = 19001
LAMBDA_PLATFORM = os.environ.get("DISCORD_TEST_PLATFORM", "linux/arm64")
RUNTIME_IMAGE = os.environ.get(
    "DISCORD_TEST_RUNTIME_IMAGE", "public.ecr.aws/lambda/provided:al2023"
)
RUNTIME_COMMAND = shlex.split(os.environ.get("DISCORD_TEST_CONTAINER_CMD", "bootstrap"))
STARTUP_ATTEMPTS = int(os.environ.get("DISCORD_TEST_STARTUP_ATTEMPTS", "120"))
STARTUP_DELAY_SECONDS = float(os.environ.get("DISCORD_TEST_STARTUP_DELAY_SECONDS", "0.5"))
KEEP_FAILED_CONTAINERS = os.environ.get("DISCORD_TEST_KEEP_FAILED_CONTAINERS", "0") == "1"


class TestFailure(RuntimeError):
    pass


def run(cmd, *, check=True, capture_output=True, input_bytes=None):
    return subprocess.run(
        cmd,
        check=check,
        cwd=REPO_ROOT,
        input=input_bytes,
        capture_output=capture_output,
        text=False,
    )


def http_json(method, url, payload=None):
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    request = urllib.request.Request(url, data=data, method=method)
    request.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(request, timeout=10) as response:
        body = response.read().decode("utf-8")
        return response.status, json.loads(body) if body else None


def assert_equal(actual, expected, message):
    if actual != expected:
        raise TestFailure(f"{message}: expected {expected!r}, got {actual!r}")


def assert_in(needle, haystack, message):
    if needle not in haystack:
        raise TestFailure(f"{message}: missing {needle!r} in {haystack!r}")


def assert_not_in(needle, haystack, message):
    if needle in haystack:
        raise TestFailure(f"{message}: unexpected {needle!r} in {haystack!r}")


def reset_mock(responses=None):
    http_json(
        "POST",
        f"http://127.0.0.1:{MOCK_PORT}/__reset",
        {"responses": responses or {}},
    )


def get_logs():
    _, payload = http_json("GET", f"http://127.0.0.1:{MOCK_PORT}/__logs")
    return payload


def get_discord_patches():
    _, payload = http_json("GET", f"http://127.0.0.1:{MOCK_PORT}/__discord_patches")
    return payload


def extract_package(zip_name, destination):
    source = PACKAGED_DIR / zip_name
    if not source.exists():
        raise TestFailure(f"missing packaged Lambda: {source}")

    run(["unzip", "-q", str(source), "-d", str(destination)])
    bootstrap = destination / "bootstrap"
    if not bootstrap.exists():
        raise TestFailure(f"package {source} did not contain bootstrap")
    bootstrap.chmod(0o755)


def wait_for_lambda(port):
    last_error = None
    for _ in range(STARTUP_ATTEMPTS):
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=2):
                pass
            return
        except Exception as exc:
            last_error = exc
            time.sleep(STARTUP_DELAY_SECONDS)
    raise TestFailure(f"lambda runtime did not become ready on port {port}: {last_error}")


class LambdaContainer:
    def __init__(self, zip_name, env):
        self.zip_name = zip_name
        self.port = None
        self.env = env
        self.tempdir = None
        self.container_name = None
        self.keep_container = False

    def _read_container_logs(self):
        if self.container_name is None:
            return ""

        result = run(["docker", "logs", self.container_name], check=False)
        output = b"".join(
            chunk for chunk in ((result.stdout or b""), (result.stderr or b"")) if chunk
        )
        return output.decode("utf-8", errors="replace").strip()

    def __enter__(self):
        self.tempdir = Path(tempfile.mkdtemp(prefix="discord-lambda-"))
        extract_package(self.zip_name, self.tempdir)
        self.port = reserve_port()
        self.container_name = f"discord-test-{self.port}-{int(time.time() * 1000)}"

        cmd = [
            "docker",
            "run",
            "-d",
            "--platform",
            LAMBDA_PLATFORM,
            "--name",
            self.container_name,
            "-p",
            f"{self.port}:8080",
            "-v",
            f"{self.tempdir}:/var/runtime:ro",
        ]

        for key, value in self.env.items():
            cmd.extend(["-e", f"{key}={value}"])

        cmd.extend([RUNTIME_IMAGE, *RUNTIME_COMMAND])
        result = run(cmd)
        container_id = result.stdout.decode("utf-8").strip()
        if not container_id:
            raise TestFailure(f"failed to start container for {self.zip_name}")

        try:
            wait_for_lambda(self.port)
        except Exception as exc:
            logs = self._read_container_logs()
            self.keep_container = KEEP_FAILED_CONTAINERS
            details = str(exc)
            if logs:
                details += f"\nContainer logs for {self.container_name}:\n{logs}"
            if self.keep_container:
                details += f"\nRetaining failed container: {self.container_name}"
            else:
                run(["docker", "rm", "-f", self.container_name], check=False)
            raise TestFailure(details) from exc
        return self

    def __exit__(self, exc_type, exc, tb):
        if self.container_name is not None and not self.keep_container:
            run(["docker", "rm", "-f", self.container_name], check=False)
        if self.tempdir is not None:
            shutil.rmtree(self.tempdir, ignore_errors=True)

    def invoke(self, payload):
        request = urllib.request.Request(
            f"http://127.0.0.1:{self.port}/2015-03-31/functions/function/invocations",
            data=json.dumps(payload).encode("utf-8"),
            method="POST",
            headers={"Content-Type": "application/json"},
        )
        last_exc = None
        for _ in range(3):
            try:
                with urllib.request.urlopen(request, timeout=10) as response:
                    return response.status, response.read().decode("utf-8")
            except urllib.error.HTTPError as error:
                return error.code, error.read().decode("utf-8")
            except Exception as exc:
                last_exc = exc
                time.sleep(0.2)

        logs = self._read_container_logs()
        self.keep_container = KEEP_FAILED_CONTAINERS
        details = f"invoke failed for {self.zip_name}: {last_exc}"
        if logs:
            details += f"\nContainer logs for {self.container_name}:\n{logs}"
        if self.keep_container:
            details += f"\nRetaining failed container: {self.container_name}"
        raise TestFailure(details) from last_exc


def generate_ed25519_keypair(tempdir):
    key_path = tempdir / "discord-key.pem"
    run(["openssl", "genpkey", "-algorithm", "Ed25519", "-out", str(key_path)])
    public_der = run(
        ["openssl", "pkey", "-in", str(key_path), "-pubout", "-outform", "DER"]
    ).stdout
    public_key_hex = public_der[-32:].hex()
    return key_path, public_key_hex


def reserve_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        return sock.getsockname()[1]


def sign_message(key_path, message):
    tempdir = key_path.parent
    message_path = tempdir / "message.bin"
    signature_path = tempdir / "signature.bin"
    message_path.write_bytes(message)
    run(
        [
            "openssl",
            "pkeyutl",
            "-sign",
            "-rawin",
            "-inkey",
            str(key_path),
            "-in",
            str(message_path),
            "-out",
            str(signature_path),
        ]
    )
    return signature_path.read_bytes().hex()


def make_ingress_event(key_path, body, *, timestamp=None, is_base64_encoded=False):
    timestamp = str(int(time.time())) if timestamp is None else timestamp
    signature = sign_message(key_path, (timestamp + body).encode("utf-8"))
    event_body = base64.b64encode(body.encode("utf-8")).decode("ascii") if is_base64_encoded else body
    return {
        "headers": {
            "x-signature-ed25519": signature,
            "x-signature-timestamp": timestamp,
        },
        "body": event_body,
        "isBase64Encoded": is_base64_encoded,
    }


def parse_json(text, context):
    try:
        return json.loads(text)
    except json.JSONDecodeError as exc:
        raise TestFailure(f"{context}: expected JSON, got {text!r}") from exc


def docker_preflight():
    try:
        run(["docker", "ps"])
    except FileNotFoundError as exc:
        raise TestFailure(
            "docker is not installed or not in PATH; install Docker Desktop and retry"
        ) from exc
    except subprocess.CalledProcessError as exc:
        stderr = (exc.stderr or b"").decode("utf-8", errors="replace").strip()
        raise TestFailure(
            "docker daemon is not reachable; start Docker Desktop and retry"
            + (f" ({stderr})" if stderr else "")
        ) from exc

    try:
        run(
            [
                "docker",
                "run",
                "--rm",
                "--platform",
                LAMBDA_PLATFORM,
                "--entrypoint",
                "/bin/sh",
                RUNTIME_IMAGE,
                "-lc",
                "echo preflight-ok",
            ]
        )
    except subprocess.CalledProcessError as exc:
        stderr = (exc.stderr or b"").decode("utf-8", errors="replace").strip()
        raise TestFailure(
            "docker runtime preflight failed for AWS Lambda image/platform "
            f"({RUNTIME_IMAGE}, {LAMBDA_PLATFORM})"
            + (f": {stderr}" if stderr else "")
        ) from exc


def run_ingress_tests():
    with tempfile.TemporaryDirectory(prefix="discord-ingress-test-") as tempdir_name:
        tempdir = Path(tempdir_name)
        key_path, public_key_hex = generate_ed25519_keypair(tempdir)
        positive_env = {
            "AWS_REGION": "us-east-1",
            "AWS_ACCESS_KEY_ID": "test",
            "AWS_SECRET_ACCESS_KEY": "test",
            "AWS_SESSION_TOKEN": "test",
            "AWS_EC2_METADATA_DISABLED": "true",
            "AWS_LAMBDA_ENDPOINT": f"http://host.docker.internal:{MOCK_PORT}",
            "DISCORD_PUBLIC_KEY": public_key_hex,
        }

        with LambdaContainer("discord-interactions.zip", positive_env) as container:
            ping_body = json.dumps({"type": 1})
            status, text = container.invoke(make_ingress_event(key_path, ping_body))
            payload = parse_json(text, "ingress ping")
            assert_equal(status, 200, "ingress ping HTTP status")
            assert_equal(payload["statusCode"], 200, "ingress ping proxy status")
            assert_equal(json.loads(payload["body"])["type"], 1, "ingress ping body")

            status, text = container.invoke(
                make_ingress_event(key_path, ping_body, is_base64_encoded=True)
            )
            payload = parse_json(text, "ingress base64 ping")
            assert_equal(payload["statusCode"], 200, "ingress base64 ping proxy status")
            assert_equal(json.loads(payload["body"])["type"], 1, "ingress base64 ping body")

            stale_timestamp = str(int(time.time()) - 300)
            status, text = container.invoke(
                make_ingress_event(key_path, ping_body, timestamp=stale_timestamp)
            )
            payload = parse_json(text, "ingress stale timestamp")
            assert_equal(payload["statusCode"], 401, "ingress stale timestamp proxy status")

            reset_mock()
            command_body = json.dumps({"type": 2, "data": {"name": "admin"}})
            status, text = container.invoke(make_ingress_event(key_path, command_body))
            payload = parse_json(text, "ingress application command")
            assert_equal(status, 200, "ingress application command HTTP status")
            assert_equal(json.loads(payload["body"])["type"], 5, "ingress deferred ack")
            logs = get_logs()
            assert_equal(len(logs), 1, "ingress application command invoke count")
            assert_equal(
                logs[0]["function_name"],
                "discord-application-command-handler",
                "ingress application command target",
            )

            reset_mock()
            component_body = json.dumps({"type": 3, "data": {"custom_id": "pager:1"}})
            status, text = container.invoke(make_ingress_event(key_path, component_body))
            payload = parse_json(text, "ingress component")
            assert_equal(status, 200, "ingress component HTTP status")
            assert_equal(json.loads(payload["body"])["type"], 6, "ingress component ack")
            logs = get_logs()
            assert_equal(logs[0]["function_name"], "discord-message-component-handler", "ingress component target")

            reset_mock()
            modal_body = json.dumps({"type": 5, "data": {"custom_id": "feedback:1"}})
            status, text = container.invoke(make_ingress_event(key_path, modal_body))
            payload = parse_json(text, "ingress modal")
            assert_equal(status, 200, "ingress modal HTTP status")
            assert_equal(json.loads(payload["body"])["type"], 5, "ingress modal ack")
            logs = get_logs()
            assert_equal(logs[0]["function_name"], "discord-modal-handler", "ingress modal target")

            reset_mock(
                {
                    "discord-autocomplete-handler": {
                        "type": 8,
                        "data": {"choices": [{"name": "alpha", "value": "alpha"}]},
                    }
                }
            )
            autocomplete_body = json.dumps(
                {
                    "type": 4,
                    "data": {
                        "name": "admin",
                        "options": [{"type": 3, "name": "query", "focused": True}],
                    },
                }
            )
            status, text = container.invoke(make_ingress_event(key_path, autocomplete_body))
            payload = parse_json(text, "ingress autocomplete")
            assert_equal(status, 200, "ingress autocomplete HTTP status")
            body = json.loads(payload["body"])
            assert_equal(body["type"], 8, "ingress autocomplete body type")
            assert_equal(body["data"]["choices"][0]["value"], "alpha", "ingress autocomplete choice")

        negative_env = dict(positive_env)
        with LambdaContainer("discord-interactions.zip", negative_env) as container:
            invalid_event = make_ingress_event(key_path, ping_body)
            invalid_event["headers"]["x-signature-ed25519"] = "00" * 64
            status, text = container.invoke(invalid_event)
            payload = parse_json(text, "ingress invalid signature")
            assert_equal(status, 200, "ingress invalid signature HTTP status")
            assert_equal(payload["statusCode"], 401, "ingress invalid signature proxy status")


def run_application_command_tests():
    reset_mock()
    env = {
        "AWS_REGION": "us-east-1",
        "AWS_ACCESS_KEY_ID": "test",
        "AWS_SECRET_ACCESS_KEY": "test",
        "AWS_SESSION_TOKEN": "test",
        "AWS_EC2_METADATA_DISABLED": "true",
        "AWS_LAMBDA_ENDPOINT": f"http://host.docker.internal:{MOCK_PORT}",
        "DISCORD_COMMAND_ROUTES": json.dumps(route_map(REPO_ROOT, "commands")),
    }
    with LambdaContainer("discord-application-command-handler.zip", env) as container:
        payload = {
            "type": 2,
            "data": {
                "name": "admin",
                "options": [{"type": 1, "name": "ban", "options": [{"type": 3, "name": "user", "value": "42"}]}],
            },
        }
        status, text = container.invoke(payload)
        assert_equal(status, 200, "application handler HTTP status")
        assert_equal(parse_json(text, "application handler success")["ok"], True, "application handler success payload")
        logs = get_logs()
        assert_equal(logs[0]["function_name"], "discord-cmd-admin-ban", "application handler route")

        reset_mock()
        status, text = container.invoke(
            {
                "type": 2,
                "data": {
                    "name": "nitrado",
                    "options": [{"type": 1, "name": "ping"}],
                },
            }
        )
        assert_equal(status, 200, "nitrado ping application handler HTTP status")
        logs = get_logs()
        assert_equal(logs[0]["function_name"], "discord-cmd-nitrado-ping", "nitrado ping route")

        reset_mock()
        status, text = container.invoke(
            {
                "type": 2,
                "data": {
                    "name": "nitrado",
                    "options": [{"type": 1, "name": "maintenance"}],
                },
            }
        )
        assert_equal(status, 200, "nitrado maintenance application handler HTTP status")
        logs = get_logs()
        assert_equal(logs[0]["function_name"], "discord-cmd-nitrado-maintenance", "nitrado maintenance route")

        reset_mock()
        status, text = container.invoke(
            {
                "type": 2,
                "data": {
                    "name": "nitrado",
                    "options": [{"type": 1, "name": "version"}],
                },
            }
        )
        assert_equal(status, 200, "nitrado version application handler HTTP status")
        logs = get_logs()
        assert_equal(logs[0]["function_name"], "discord-cmd-nitrado-version", "nitrado version route")

        reset_mock()
        status, text = container.invoke(
            {
                "type": 2,
                "data": {
                    "name": "nitrado",
                    "options": [{"type": 1, "name": "server-list"}],
                },
            }
        )
        assert_equal(status, 200, "nitrado server-list application handler HTTP status")
        logs = get_logs()
        assert_equal(logs[0]["function_name"], "discord-cmd-nitrado-server-list", "nitrado server-list route")

        reset_mock()
        status, text = container.invoke(
            {
                "type": 2,
                "data": {
                    "name": "nitrado",
                    "options": [
                        {
                            "type": 1,
                            "name": "server-info",
                            "options": [{"type": 3, "name": "service_id", "value": "123"}],
                        }
                    ],
                },
            }
        )
        assert_equal(status, 200, "nitrado server-info application handler HTTP status")
        logs = get_logs()
        assert_equal(logs[0]["function_name"], "discord-cmd-nitrado-server-info", "nitrado server-info route")

        for action in ("server-start", "server-stop", "server-restart"):
            reset_mock()
            status, text = container.invoke(
                {
                    "type": 2,
                    "data": {
                        "name": "nitrado",
                        "options": [
                            {
                                "type": 1,
                                "name": action,
                                "options": [{"type": 3, "name": "service_id", "value": "123"}],
                            }
                        ],
                    },
                }
            )
            assert_equal(status, 200, f"nitrado {action} application handler HTTP status")
            logs = get_logs()
            assert_equal(logs[0]["function_name"], f"discord-cmd-nitrado-{action}", f"nitrado {action} route")

        reset_mock()
        status, text = container.invoke(
            {
                "type": 2,
                "data": {
                    "name": "ping",
                },
            }
        )
        assert_equal(status, 200, "example ping application handler HTTP status")
        logs = get_logs()
        assert_equal(logs[0]["function_name"], "discord-cmd-example-ping", "example ping route")

        reset_mock()
        status, text = container.invoke({"type": 2, "data": {}})
        assert_equal(status, 200, "application handler missing name HTTP status")
        assert_in("internal error", text, "application handler missing name error")


def run_message_component_tests():
    reset_mock()
    env = {
        "AWS_REGION": "us-east-1",
        "AWS_ACCESS_KEY_ID": "test",
        "AWS_SECRET_ACCESS_KEY": "test",
        "AWS_SESSION_TOKEN": "test",
        "AWS_EC2_METADATA_DISABLED": "true",
        "AWS_LAMBDA_ENDPOINT": f"http://host.docker.internal:{MOCK_PORT}",
        "DISCORD_COMPONENT_ROUTES": json.dumps(route_map(REPO_ROOT, "components")),
    }
    with LambdaContainer("discord-message-component-handler.zip", env) as container:
        status, text = container.invoke({"type": 3, "data": {"custom_id": "pager:2"}})
        assert_equal(status, 200, "component handler HTTP status")
        assert_equal(parse_json(text, "component handler success")["ok"], True, "component handler success payload")
        logs = get_logs()
        assert_equal(logs[0]["function_name"], "discord-component-pager", "component handler route")

        reset_mock()
        status, text = container.invoke({"type": 3, "data": {}})
        assert_equal(status, 200, "component handler missing custom_id HTTP status")
        assert_in("internal error", text, "component handler missing custom_id error")

        reset_mock()
        status, text = container.invoke({"type": 3, "data": {"custom_id": "../evil:1"}})
        assert_equal(status, 200, "component handler invalid custom_id HTTP status")
        assert_in("internal error", text, "component handler invalid custom_id error")
        assert_equal(get_logs(), [], "component handler invalid custom_id invoke count")


def run_modal_tests():
    reset_mock()
    env = {
        "AWS_REGION": "us-east-1",
        "AWS_ACCESS_KEY_ID": "test",
        "AWS_SECRET_ACCESS_KEY": "test",
        "AWS_SESSION_TOKEN": "test",
        "AWS_EC2_METADATA_DISABLED": "true",
        "AWS_LAMBDA_ENDPOINT": f"http://host.docker.internal:{MOCK_PORT}",
    }
    with LambdaContainer("discord-modal-handler.zip", env) as container:
        status, text = container.invoke({"type": 5, "data": {"custom_id": "feedback:2"}})
        assert_equal(status, 200, "modal handler HTTP status")
        assert_equal(parse_json(text, "modal handler success")["ok"], True, "modal handler success payload")
        logs = get_logs()
        assert_equal(logs[0]["function_name"], "discord-modal-feedback", "modal handler route")

        reset_mock()
        status, text = container.invoke({"type": 5, "data": {}})
        assert_equal(status, 200, "modal handler missing custom_id HTTP status")
        assert_in("internal error", text, "modal handler missing custom_id error")

        reset_mock()
        status, text = container.invoke({"type": 5, "data": {"custom_id": "foo*bar"}})
        assert_equal(status, 200, "modal handler invalid custom_id HTTP status")
        assert_in("internal error", text, "modal handler invalid custom_id error")
        assert_equal(get_logs(), [], "modal handler invalid custom_id invoke count")


def run_autocomplete_tests():
    reset_mock(
        {
            "discord-autocomplete-admin-ban-user": {
                "type": 8,
                "data": {"choices": [{"name": "user-42", "value": "42"}]},
            }
        }
    )
    env = {
        "AWS_REGION": "us-east-1",
        "AWS_ACCESS_KEY_ID": "test",
        "AWS_SECRET_ACCESS_KEY": "test",
        "AWS_SESSION_TOKEN": "test",
        "AWS_EC2_METADATA_DISABLED": "true",
        "AWS_LAMBDA_ENDPOINT": f"http://host.docker.internal:{MOCK_PORT}",
    }
    with LambdaContainer("discord-autocomplete-handler.zip", env) as container:
        payload = {
            "type": 4,
            "data": {
                "name": "admin",
                "options": [
                    {
                        "type": 1,
                        "name": "ban",
                        "options": [{"type": 3, "name": "user", "focused": True, "value": "u"}],
                    }
                ],
            },
        }
        status, text = container.invoke(payload)
        assert_equal(status, 200, "autocomplete handler HTTP status")
        response = parse_json(text, "autocomplete handler response")
        assert_equal(response["type"], 8, "autocomplete handler body type")
        assert_equal(response["data"]["choices"][0]["value"], "42", "autocomplete handler choice")
        logs = get_logs()
        assert_equal(logs[0]["function_name"], "discord-autocomplete-admin-ban-user", "autocomplete handler route")

        reset_mock()
        status, text = container.invoke({"type": 4, "data": {"name": "admin"}})
        assert_equal(status, 200, "autocomplete missing focused option HTTP status")
        assert_in("no focused option", text, "autocomplete missing focused option error")

        reset_mock(
            {
                "discord-autocomplete-admin-ban-user": {
                    "__sleep_seconds": 3.5,
                    "type": 8,
                    "data": {"choices": [{"name": "late", "value": "late"}]},
                }
            }
        )
        start = time.monotonic()
        status, text = container.invoke(payload)
        elapsed = time.monotonic() - start
        response = parse_json(text, "autocomplete slow worker fallback")
        assert_equal(status, 200, "autocomplete slow worker HTTP status")
        assert_equal(response, {"type": 8, "data": {"choices": []}}, "autocomplete slow worker fallback")
        if elapsed >= 3.0:
            raise TestFailure(f"autocomplete slow worker exceeded Discord deadline: {elapsed:.3f}s")


def assert_before(text, earlier, later, message):
    earlier_index = text.find(earlier)
    later_index = text.find(later)
    if earlier_index == -1 or later_index == -1 or earlier_index >= later_index:
        raise TestFailure(
            f"{message}: expected {earlier!r} before {later!r}"
        )


def run_nitrado_command_response_tests():
    validate_module_manifests(REPO_ROOT)

    sign_in_source = (
        REPO_ROOT / "modules" / "nitrado" / "lambdas" / "commands" / "discord-cmd-nitrado-sign-in" / "main.cpp"
    ).read_text(encoding="utf-8")
    assert_not_in('"Link URL"', sign_in_source, "nitrado sign-in raw URL field")
    assert_not_in("guild_id.empty()", sign_in_source, "nitrado sign-in must work in bot DMs")
    assert_in("discord_interaction_user_id", sign_in_source, "nitrado sign-in supports DM user payloads")
    assert_not_in('{"fields"', sign_in_source, "nitrado sign-in embed fields block")
    assert_in('"Sign in to Nitrado"', sign_in_source, "nitrado sign-in embed title")
    assert_in('"Sign in with Nitrado"', sign_in_source, "nitrado sign-in button label")

    response_helper = (
        REPO_ROOT / "modules" / "nitrado" / "lambdas" / "commands" / "discord-response.hpp"
    ).read_text(encoding="utf-8")
    assert_in(
        'interaction.value("user", nlohmann::json::object()).value("id", "")',
        response_helper,
        "Discord interaction user helper falls back to DM user payload",
    )

    command_manifest = json.loads(
        (REPO_ROOT / "modules" / "nitrado" / "discord.commands.json").read_text(encoding="utf-8")
    )
    assert_equal(
        command_manifest[0].get("default_member_permissions"),
        "8",
        "nitrado guild commands require Discord administrator permission",
    )

    embed_helper = (
        REPO_ROOT / "modules" / "nitrado" / "lambdas" / "commands" / "nitrado-discord-embed.hpp"
    ).read_text(encoding="utf-8")
    assert_in("inline constexpr int error_embed_color = 0xE74C3C;", embed_helper, "error embed color")
    assert_in("ephemeral_error_embed_payload", embed_helper, "error embed payload helper")
    assert_in("HTTP 401", embed_helper, "friendly errors detect expired Nitrado credentials")
    assert_in("run `/nitrado sign-in` again", embed_helper, "friendly errors tell users to sign in again")
    assert_in("DISCORD_BOT_TOKEN", embed_helper, "friendly errors identify missing Discord bot token config")
    assert_in("Unknown Guild", embed_helper, "friendly errors identify Discord guild visibility problems")
    assert_in("Please ensure the bot is in the server", embed_helper, "friendly errors explain Discord bot install issues")
    assert_in("discord_bot_invite_url", embed_helper, "friendly errors can build Discord bot invite URL")
    assert_in("invite_bot_components", embed_helper, "friendly errors can include Discord bot invite button")
    assert_in("ephemeral_error_embed_payload_for_error", embed_helper, "friendly errors can build contextual error payloads")
    assert_in("scope=bot+applications.commands", embed_helper, "Discord bot invite includes bot and slash command scopes")
    assert_in("permissions=268435472", embed_helper, "Discord bot invite requests manage channels and manage roles")
    assert_in('{"style", 5}', embed_helper, "Discord bot invite is a link button component")
    assert_in('"Invite bot"', embed_helper, "Discord bot invite button label")
    assert_not_in("[Invite the bot to this server]", embed_helper, "Discord bot invite should be a component instead of markdown")

    oauth_common_source = (
        REPO_ROOT / "modules" / "nitrado" / "lambdas" / "nitrado" / "nitrado-oauth-common.hpp"
    ).read_text(encoding="utf-8")
    assert_in(
        "Discord bot cannot access guild",
        oauth_common_source,
        "Discord guild lookup errors are classified separately from Nitrado/OAuth errors",
    )

    list_accounts_source = (
        REPO_ROOT
        / "modules" / "nitrado" / "lambdas" / "commands"
        / "discord-cmd-nitrado-list-accounts"
        / "main.cpp"
    ).read_text(encoding="utf-8")
    assert_in(
        'ephemeral_error_embed_payload("Unable to List Accounts"',
        list_accounts_source,
        "list-accounts error embed helper",
    )
    assert_not_in('{{"content", message}, {"flags", 64}}', list_accounts_source, "list-accounts plain content payload")
    assert_before(
        list_accounts_source,
        "try {",
        'require_env("NITRADO_OAUTH_TABLE_NAME")',
        "list-accounts user token table require_env inside try",
    )
    assert_not_in("Token ID:", list_accounts_source, "list-accounts must not expose token ids")
    assert_not_in("✅", list_accounts_source, "list-accounts account row should not use an ambiguous check mark")
    assert_not_in("🔗", list_accounts_source, "list-accounts account row should not use an account emoji")
    assert_in("Linked accounts:", list_accounts_source, "list-accounts account label")
    assert_in('<< "\\n**" << account.nitrado_user_id << "**"', list_accounts_source, "list-accounts bold account id")
    assert_in('"**"', list_accounts_source, "list-accounts bold account id terminator")
    assert_in('" - "', list_accounts_source, "list-accounts separates account id and username")
    assert_in("Servers: ", list_accounts_source, "list-accounts shows linked server list")
    assert_in('output << "this server"', list_accounts_source, "list-accounts labels the current guild without exposing its id")
    assert_in("discord_guild_label", list_accounts_source, "list-accounts resolves server names")
    assert_in("list_servers_for_linked_account", list_accounts_source, "list-accounts queries account server links")
    assert_in("Current server: **not linked**", list_accounts_source, "list-accounts bold current server unlink status")
    assert_in("list_linked_nitrado_accounts", list_accounts_source, "list-accounts queries all user credentials")
    assert_in(
        "get_server_metadata",
        list_accounts_source,
        "list-accounts reports current guild-to-Discord-user link state",
    )

    link_account_source = (
        REPO_ROOT
        / "modules" / "nitrado" / "lambdas" / "commands"
        / "discord-cmd-nitrado-link"
        / "main.cpp"
    ).read_text(encoding="utf-8")
    assert_in("option_value", link_account_source, "link account parses optional Nitrado account option")
    assert_in('require_env("NITRADO_OAUTH_TABLE_NAME")', link_account_source, "link account reads the token table")
    assert_in("list_linked_nitrado_accounts", link_account_source, "link account checks the invoking user's credentials")
    assert_in("ensure_server_owner", link_account_source, "link account creates or replaces the guild-to-Discord-user link")
    assert_not_in("delete_all_server_accounts", link_account_source, "link account preserves other admin account links")
    assert_in("upsert_server_account", link_account_source, "link account writes the selected server account")
    assert_not_in("set_server_account_enabled", link_account_source, "link account must not require an existing server binding")
    assert_in('"Unable to Link Account"', link_account_source, "link account error title")
    assert_in('"Linked this server to Nitrado account "', link_account_source, "link account success copy")

    unlink_account_source = (
        REPO_ROOT
        / "modules" / "nitrado" / "lambdas" / "commands"
        / "discord-cmd-nitrado-unlink"
        / "main.cpp"
    ).read_text(encoding="utf-8")
    assert_not_in("option_value", unlink_account_source, "unlink should not parse a Nitrado account option")
    assert_in("delete_all_server_accounts", unlink_account_source, "unlink removes selected server accounts")
    assert_in("delete_server_link", unlink_account_source, "unlink removes the guild-to-Discord-user link")
    assert_not_in("require_server_owner", unlink_account_source, "unlink can be run by any server admin")
    assert_in('"Unable to Unlink Account"', unlink_account_source, "unlink account error title")
    assert_in('"Removed this server', unlink_account_source, "unlink account success copy")

    forget_source = (
        REPO_ROOT
        / "modules" / "nitrado" / "lambdas" / "commands"
        / "discord-cmd-nitrado-forget"
        / "main.cpp"
    ).read_text(encoding="utf-8")
    assert_in(
        'option_value(option.value("options", json::array()), key)',
        forget_source,
        "forget nested option parser",
    )
    assert_in('"nitrado_user_id"', forget_source, "forget requires Nitrado user ID")
    assert_in("discord_interaction_user_id", forget_source, "forget supports DM user payloads")
    assert_in("delete_linked_nitrado_account", forget_source, "forget uses credential delete helper")
    assert_in('"Unable to Forget Credentials"', forget_source, "forget error title")
    assert_in('"Deleted Nitrado credentials "', forget_source, "forget success copy")

    oauth_common_source = (
        REPO_ROOT / "modules" / "nitrado" / "lambdas" / "nitrado" / "nitrado-oauth-common.hpp"
    ).read_text(encoding="utf-8")
    assert_in("access_token_ciphertext", oauth_common_source, "OAuth access token ciphertext storage")
    assert_in("refresh_token_ciphertext", oauth_common_source, "OAuth refresh token ciphertext storage")
    assert_in("encrypt_oauth_token", oauth_common_source, "OAuth token encryption helper")
    assert_in("decrypt_oauth_token", oauth_common_source, "OAuth token decryption helper")
    assert_in("key_sk_for_token", oauth_common_source, "OAuth token account-specific sort key")
    assert_in('kTokenSortKeyPrefix = "NITRADO#"', oauth_common_source, "OAuth token sort key prefix")
    assert_in("list_linked_nitrado_accounts", oauth_common_source, "OAuth linked account listing helper")
    assert_in("delete_all_server_accounts", oauth_common_source, "server account selection cleanup helper")
    assert_in("delete_server_account", oauth_common_source, "single server account cleanup helper")
    assert_in("admin_discord_user_ids", oauth_common_source, "server account rows track admin contributors")
    assert_in("update_server_account_admins", oauth_common_source, "server account contributor pruning helper")
    assert_in("discord_member_is_guild_admin", oauth_common_source, "Discord admin status helper")
    assert_in("list_admin_server_accounts", oauth_common_source, "admin-filtered server account helper")
    assert_in("discord-user-account-index", oauth_common_source, "server bindings reverse account lookup index")
    assert_in("list_servers_for_linked_account", oauth_common_source, "server bindings account-to-server listing helper")
    assert_in("get_token_record(const std::string& table_name,", oauth_common_source, "OAuth account-specific token getter")
    assert_in("delete_linked_nitrado_account", oauth_common_source, "linked account delete helper")
    assert_in("DynamoDB DeleteItem for linked account failed", oauth_common_source, "linked account delete error")
    assert_in(
        "This Nitrado account needs to be linked again before it can be used.",
        oauth_common_source,
        "OAuth undecryptable token relink message",
    )

    terraform_source = (
        REPO_ROOT / "modules" / "nitrado" / "terraform" / "main.tf"
    ).read_text(encoding="utf-8")
    assert_in('resource "aws_kms_key" "oauth_tokens"', terraform_source, "OAuth token KMS key")
    assert_in("NITRADO_OAUTH_KMS_KEY_ID", terraform_source, "OAuth token KMS environment")
    assert_in("kms:Encrypt", terraform_source, "OAuth token KMS encrypt permission")
    assert_in("kms:Decrypt", terraform_source, "OAuth token KMS decrypt permission")
    assert_in('"dynamodb:DeleteItem"', terraform_source, "token table delete permission")
    assert_in('"dynamodb:Query"', terraform_source, "token table query permission")
    assert_in(
        '"discord-cmd-nitrado-forget"',
        terraform_source,
        "forget terraform resource",
    )
    assert_in('"discord-cmd-nitrado-link"', terraform_source, "link terraform resource")
    link_tf_start = terraform_source.find('"discord-cmd-nitrado-link"')
    link_tf_end = terraform_source.find('"discord-cmd-nitrado-unlink"', link_tf_start)
    link_tf = terraform_source[link_tf_start:link_tf_end]
    assert_in(
        "local.common_env",
        link_tf,
        "link terraform token table environment",
    )
    assert_in(
        "local.common_env",
        link_tf,
        "link terraform token table IAM dependency",
    )
    list_accounts_tf_start = terraform_source.find('"discord-cmd-nitrado-list-accounts"')
    list_accounts_tf_end = terraform_source.find('"discord-cmd-nitrado-server-list"', list_accounts_tf_start)
    list_accounts_tf = terraform_source[list_accounts_tf_start:list_accounts_tf_end]
    assert_in(
        "local.common_env",
        list_accounts_tf,
        "list-accounts terraform token table environment",
    )
    assert_in(
        "DISCORD_BOT_TOKEN",
        list_accounts_tf,
        "list-accounts terraform token table IAM dependency",
    )
    assert_not_in("discord_cmd_nitrado_enable_all", terraform_source, "enable-all terraform resource removed")
    module_variables_source = (
        REPO_ROOT / "modules" / "nitrado" / "terraform" / "variables.tf"
    ).read_text(encoding="utf-8")
    assert_in(
        'variable "discord_bot_token"',
        module_variables_source,
        "Discord bot token module terraform variable",
    )
    module_tfvars_example = (
        REPO_ROOT / "modules" / "nitrado" / "terraform" / "terraform.tfvars.example"
    ).read_text(encoding="utf-8")
    assert_in(
        "discord_bot_token",
        module_tfvars_example,
        "Discord bot token module terraform example",
    )

    server_list_source = (
        REPO_ROOT
        / "modules" / "nitrado" / "lambdas" / "commands"
        / "discord-cmd-nitrado-server-list"
        / "main.cpp"
    ).read_text(encoding="utf-8")
    assert_in(
        'ephemeral_error_embed_payload_for_error(\n                         "Unable to Load Gameservers"',
        server_list_source,
        "server-list error embed helper",
    )
    assert_in(
        "ephemeral_error_embed_payload_for_error",
        server_list_source,
        "server-list can include bot invite component in Discord guild visibility errors",
    )
    assert_not_in("must be used in a linked Discord server", server_list_source, "server-list works in DMs")
    assert_before(
        server_list_source,
        "try {",
        'require_env("NITRADO_OAUTH_SERVER_TABLE_NAME")',
        "server-list require_env inside try",
    )

    component_source = (
        REPO_ROOT
        / "modules" / "nitrado" / "lambdas" / "components"
        / "discord-component-nitrado-server-list"
        / "main.cpp"
    ).read_text(encoding="utf-8")
    assert_in(
        'ephemeral_error_embed_payload_for_error(\n            "Unable to Change Gameserver Page"',
        component_source,
        "server-list component error embed helper",
    )
    assert_in("discord_interaction_user_id", component_source, "server-list component supports DM user payloads")
    assert_in('payload["components"] = json::array();', component_source, "server-list component clears buttons on error")
    assert_in(
        "ephemeral_error_embed_payload_for_error",
        component_source,
        "server-list component can include bot invite component in Discord guild visibility errors",
    )
    assert_before(
        component_source,
        "try {",
        'require_env("NITRADO_OAUTH_SERVER_TABLE_NAME")',
        "server-list component require_env inside try",
    )

    pages_source = (
        REPO_ROOT / "modules" / "nitrado" / "lambdas" / "commands" / "nitrado-server-list-pages.hpp"
    ).read_text(encoding="utf-8")
    assert_in('"No Linked Nitrado Accounts"', pages_source, "server-list no account embed title")
    assert_in(
        "require_server_linked_discord_user",
        pages_source,
        "server-list resolves the guild's linked Discord user",
    )
    assert_in("list_admin_server_accounts(server_table, guild_id)", pages_source, "server-list uses admin-owned selected server accounts in guilds")
    assert_in(
        "list_linked_nitrado_accounts(token_table, user_id)",
        pages_source,
        "server-list loads the invoking user's accounts in DMs",
    )
    compact_pages_source = "".join(pages_source.split())
    assert_in(
        "get_valid_token_record(token_table,account.discord_user_id,account.nitrado_user_id)",
        compact_pages_source,
        "server-list fetches account-specific token",
    )
    assert_in('"No Services Available"', pages_source, "server-list empty services embed title")
    assert_in('service.value("details", json::object())', pages_source, "server-list reads service details")
    assert_in('optional_string(service, "type_human")', pages_source, "server-list type_human fallback")
    assert_in('":green_circle:"', pages_source, "server-list green status indicator")
    assert_in('":yellow_circle:"', pages_source, "server-list yellow status indicator")
    assert_in('":red_circle:"', pages_source, "server-list red status indicator")
    assert_in('"suspending_in"', pages_source, "server-list suspending_in display")
    assert_in('":R>"', pages_source, "server-list Discord relative timestamp display")
    assert_in('"game_slots"', pages_source, "server-list max slots display")
    assert_in("ServiceSection", pages_source, "server-list renders each service as an embed section")
    assert_in('"Status: "', pages_source, "server-list section displays status")
    assert_in('"\\nPlayers: "', pages_source, "server-list section displays active player count")
    assert_in("service_player_count_text", pages_source, "server-list fetches active player counts")
    assert_in('"/gameservers"', pages_source, "server-list uses the gameserver query endpoint for active players")
    assert_in('"?/"', pages_source, "server-list uses ?/max players when current count is unavailable")
    assert_in('"\\nSuspends: "', pages_source, "server-list section displays suspension timing")
    assert_in("max_services_per_page = 10", pages_source, "server-list caps services per embed")
    assert_in('pages.embeds.size() > 1 ? build_buttons', pages_source, "server-list only paginates long output")
    assert_not_in('"description", description', pages_source, "server-list sections are fields, not one description")
    assert_not_in('"address"', pages_source, "server-list must not expose service address")
    assert_not_in('{"content", "No Nitrado accounts are linked to this server."}', pages_source, "server-list no accounts plain content")
    assert_not_in('{"content", "No Nitrado services were available for the linked accounts."}', pages_source, "server-list no services plain content")

    server_info_source = (
        REPO_ROOT
        / "modules" / "nitrado" / "lambdas" / "commands"
        / "discord-cmd-nitrado-server-info"
        / "main.cpp"
    ).read_text(encoding="utf-8")
    assert_in('"service_id"', server_info_source, "server-info optional service_id option")
    assert_in("option_value(option.value(\"options\"", server_info_source, "server-info nested option parser")
    assert_not_in("must be used in a linked Discord server", server_info_source, "server-info works in DMs")
    assert_in(
        "ephemeral_error_embed_payload_for_error",
        server_info_source,
        "server-info can include bot invite component in Discord guild visibility errors",
    )

    server_info_pages_source = (
        REPO_ROOT / "modules" / "nitrado" / "lambdas" / "commands" / "nitrado-server-info-pages.hpp"
    ).read_text(encoding="utf-8")
    assert_in(
        "require_server_linked_discord_user",
        server_info_pages_source,
        "server-info resolves the guild's linked Discord user",
    )
    assert_in("list_admin_server_accounts(server_table, guild_id)", server_info_pages_source, "server-info uses admin-owned selected server accounts in guilds")
    assert_in(
        "list_linked_nitrado_accounts(token_table, user_id)",
        server_info_pages_source,
        "server-info loads the invoking user's accounts in DMs",
    )
    assert_in('"/services/"', server_info_pages_source, "server-info detailed service endpoint")
    assert_in('"/gameservers"', server_info_pages_source, "server-info fetches gameserver query data for player counts")
    assert_in('"Players"', server_info_pages_source, "server-info displays current player count")
    assert_in("player_count_text", server_info_pages_source, "server-info extracts player count without exposing query details")
    assert_in("unknown_current_player_text", server_info_pages_source, "server-info falls back to unknown current over max slots")
    assert_in('"?/"', server_info_pages_source, "server-info uses ?/max players when current count is unavailable")
    assert_in("status_display_text", server_info_pages_source, "server-info formats status through a display helper")
    assert_in('status == "online"', server_info_pages_source, "server-info maps online status to green")
    assert_in('status == "offline"', server_info_pages_source, "server-info maps offline status to red")
    assert_in('status == "restarting"', server_info_pages_source, "server-info maps restarting status to yellow")
    assert_in('status == "suspended"', server_info_pages_source, "server-info maps suspended status to red")
    assert_in('"query"', server_info_pages_source, "server-info reads live query data")
    assert_not_in('"Address"', server_info_pages_source, "server-info must not expose service address")
    assert_not_in('"address"', server_info_pages_source, "server-info must not read service address")
    assert_not_in('"ip"', server_info_pages_source, "server-info must not read gameserver ip")
    assert_not_in('"port"', server_info_pages_source, "server-info must not read gameserver port")
    assert_in('"Roles"', server_info_pages_source, "server-info roles field")
    assert_in('"Timing"', server_info_pages_source, "server-info timing field")
    assert_in('"nitrado-server-info:"', server_info_pages_source, "server-info paginator custom id")
    assert_in("websocket_token", server_info_pages_source, "server-info documents hidden sensitive field")
    assert_not_in('add_field(fields, "Websocket Token"', server_info_pages_source, "server-info must not display websocket token")

    server_action_source = (
        REPO_ROOT / "modules" / "nitrado" / "lambdas" / "commands" / "nitrado-server-action.hpp"
    ).read_text(encoding="utf-8")
    assert_in('"service_id"', server_action_source, "server action requires service_id")
    assert_in("option_value(option.value(\"options\"", server_action_source, "server action nested option parser")
    assert_in("parse_service_id", server_action_source, "server action validates numeric service id")
    assert_in("require_server_linked_discord_user", server_action_source, "server action resolves guild link")
    assert_in("list_admin_server_accounts(server_table, guild_id)", server_action_source, "server action uses admin-owned selected accounts in guilds")
    assert_in("list_linked_nitrado_accounts(token_table, user_id)", server_action_source, "server action loads invoking user's accounts in DMs")
    assert_in("http_post_json_response", server_action_source, "server action posts to Nitrado control endpoint")
    assert_in('"/gameservers/" + config.action_path', server_action_source, "server action targets gameserver control path")
    assert_in("ephemeral_error_embed_payload_for_error", server_action_source, "server action uses friendly error embeds")

    for action in ("start", "stop", "restart"):
        command_source = (
            REPO_ROOT
            / "modules" / "nitrado" / "lambdas" / "commands"
            / f"discord-cmd-nitrado-server-{action}"
            / "main.cpp"
        ).read_text(encoding="utf-8")
        assert_in(f'"server-{action}"', command_source, f"server-{action} command name")
        assert_in(f'"{action}"', command_source, f"server-{action} action path")
        assert_in("nitrado_server_action::handle", command_source, f"server-{action} uses shared action handler")

    server_info_component_source = (
        REPO_ROOT
        / "modules" / "nitrado" / "lambdas" / "components"
        / "discord-component-nitrado-server-info"
        / "main.cpp"
    ).read_text(encoding="utf-8")
    assert_in("parse_requested_page(custom_id)", server_info_component_source, "server-info component parses page")
    assert_in("discord_interaction_user_id", server_info_component_source, "server-info component supports DM user payloads")
    assert_in('payload["components"] = json::array();', server_info_component_source, "server-info component clears buttons on error")
    assert_in(
        "ephemeral_error_embed_payload_for_error",
        server_info_component_source,
        "server-info component can include bot invite component in Discord guild visibility errors",
    )


def main():
    parser = argparse.ArgumentParser(description="Run local Discord Lambda regression tests.")
    parser.add_argument(
        "--suite",
        action="append",
        choices=[
            "ingress",
            "application",
            "component",
            "modal",
            "autocomplete",
            "nitrado-responses",
        ],
        help="Run only the named suite. Pass multiple times to run several suites.",
    )
    args = parser.parse_args()

    suites = args.suite or [
        "ingress",
        "application",
        "component",
        "modal",
        "autocomplete",
        "nitrado-responses",
    ]

    docker_suites = {"ingress", "application", "component", "modal", "autocomplete"}
    mock_server = None
    if any(suite in docker_suites for suite in suites):
        docker_preflight()
        mock_server = subprocess.Popen(
            [sys.executable, str(MOCK_SERVER), str(MOCK_PORT)],
            cwd=REPO_ROOT,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

    try:
        if mock_server is not None:
            time.sleep(0.5)
        for suite in suites:
            if suite == "ingress":
                run_ingress_tests()
            elif suite == "application":
                run_application_command_tests()
            elif suite == "component":
                run_message_component_tests()
            elif suite == "modal":
                run_modal_tests()
            elif suite == "autocomplete":
                run_autocomplete_tests()
            elif suite == "nitrado-responses":
                run_nitrado_command_response_tests()
    finally:
        if mock_server is not None:
            mock_server.terminate()
            try:
                mock_server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                mock_server.kill()

    print("All local Discord Lambda tests passed.")


if __name__ == "__main__":
    try:
        main()
    except TestFailure as exc:
        print(f"TEST FAILURE: {exc}", file=sys.stderr)
        sys.exit(1)
