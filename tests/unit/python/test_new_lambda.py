"""Scaffolding generator CLI tests (T4.3).

Drives ``scripts/new-lambda.py``, the ``create-discord-bot``-style DX helper
that generates a correctly named command/component/modal/autocomplete Lambda
skeleton inside a module, wires its manifest route, appends a schema stub for
command kinds, and prints a next-step checklist.

All tests operate on tmp-dir fake modules (``make_module`` from conftest) via
the script's ``--modules-root`` override. The real repository ``modules/``
directory is never read or written.
"""

import json
import subprocess
import sys
from pathlib import Path

from discord_modules import (
    check_route_consistency,
    discover_modules,
    validate_module_manifests,
)

REPO_ROOT = Path(__file__).resolve().parents[3]
SCRIPT = REPO_ROOT / "scripts" / "new-lambda.py"


def run_cli(repo_root, *args):
    """Invoke the generator against a tmp-dir modules root."""
    return subprocess.run(
        [
            sys.executable,
            str(SCRIPT),
            "--modules-root",
            str(repo_root / "modules"),
            *args,
        ],
        capture_output=True,
        text=True,
    )


def snapshot(root):
    """Map of relative-path -> bytes for every file under ``root``."""
    if not root.exists():
        return {}
    return {
        str(p.relative_to(root)): p.read_bytes()
        for p in sorted(root.rglob("*"))
        if p.is_file()
    }


def base_manifest(name="demo", **extra):
    manifest = {"name": name, "commands": "discord.commands.json"}
    manifest.update(extra)
    return manifest


def read_manifest(repo_root, module="demo"):
    path = repo_root / "modules" / module / "module.manifest.json"
    return json.loads(path.read_text(encoding="utf-8"))


# ---------------------------------------------------------------------------
# Command kind
# ---------------------------------------------------------------------------


class TestCommandKind:
    def test_creates_main_cpp_at_derived_path(self, repo_root, make_module):
        make_module("demo", base_manifest(), commands=[])
        result = run_cli(repo_root, "--module", "demo", "--kind", "command",
                         "--path", "group sub")
        assert result.returncode == 0, result.stderr
        main_cpp = (repo_root / "modules" / "demo" / "lambdas" / "commands"
                    / "discord-cmd-group-sub" / "main.cpp")
        assert main_cpp.exists()

    def test_adds_command_route(self, repo_root, make_module):
        make_module("demo", base_manifest(), commands=[])
        run_cli(repo_root, "--module", "demo", "--kind", "command",
                "--path", "group sub")
        manifest = read_manifest(repo_root)
        assert manifest["routes"]["commands"]["group sub"] == "discord-cmd-group-sub"

    def test_adds_lambda_folder_to_manifest(self, repo_root, make_module):
        make_module("demo", base_manifest(), commands=[])
        run_cli(repo_root, "--module", "demo", "--kind", "command",
                "--path", "group sub")
        manifest = read_manifest(repo_root)
        assert "lambdas/commands/discord-cmd-group-sub" in manifest["lambdas"]

    def test_generated_module_passes_full_validation(self, repo_root, make_module):
        # The generated schema stub + route + folder must satisfy BOTH
        # validate_module_manifests (T4.1) and check_route_consistency (T4.2).
        make_module("demo", base_manifest(), commands=[])
        run_cli(repo_root, "--module", "demo", "--kind", "command",
                "--path", "group sub")
        validate_module_manifests(repo_root)  # must not raise
        result = check_route_consistency(discover_modules(repo_root))
        assert result["errors"] == []

    def test_one_word_path_top_command(self, repo_root, make_module):
        make_module("demo", base_manifest(), commands=[])
        run_cli(repo_root, "--module", "demo", "--kind", "command", "--path", "ping")
        manifest = read_manifest(repo_root)
        assert manifest["routes"]["commands"]["ping"] == "discord-cmd-ping"
        validate_module_manifests(repo_root)
        assert check_route_consistency(discover_modules(repo_root))["errors"] == []

    def test_three_word_path_group_subcommand(self, repo_root, make_module):
        make_module("demo", base_manifest(), commands=[])
        run_cli(repo_root, "--module", "demo", "--kind", "command",
                "--path", "cfg user add")
        manifest = read_manifest(repo_root)
        assert manifest["routes"]["commands"]["cfg user add"] == "discord-cmd-cfg-user-add"
        validate_module_manifests(repo_root)
        assert check_route_consistency(discover_modules(repo_root))["errors"] == []

    def test_schema_stub_appended(self, repo_root, make_module):
        make_module("demo", base_manifest(), commands=[])
        run_cli(repo_root, "--module", "demo", "--kind", "command",
                "--path", "group sub")
        commands = json.loads(
            (repo_root / "modules" / "demo" / "discord.commands.json").read_text()
        )
        names = [c.get("name") for c in commands]
        assert "group" in names


# ---------------------------------------------------------------------------
# Component / modal kinds
# ---------------------------------------------------------------------------


class TestComponentKind:
    def test_component_files_and_route(self, repo_root, make_module):
        make_module("demo", base_manifest())
        result = run_cli(repo_root, "--module", "demo", "--kind", "component",
                         "--path", "vote")
        assert result.returncode == 0, result.stderr
        main_cpp = (repo_root / "modules" / "demo" / "lambdas" / "components"
                    / "discord-component-vote" / "main.cpp")
        assert main_cpp.exists()
        manifest = read_manifest(repo_root)
        assert manifest["routes"]["components"]["vote"] == "discord-component-vote"


class TestModalKind:
    def test_modal_files_and_route(self, repo_root, make_module):
        make_module("demo", base_manifest())
        result = run_cli(repo_root, "--module", "demo", "--kind", "modal",
                         "--path", "feedback")
        assert result.returncode == 0, result.stderr
        main_cpp = (repo_root / "modules" / "demo" / "lambdas" / "modals"
                    / "discord-modal-feedback" / "main.cpp")
        assert main_cpp.exists()
        manifest = read_manifest(repo_root)
        assert manifest["routes"]["modals"]["feedback"] == "discord-modal-feedback"


# ---------------------------------------------------------------------------
# Autocomplete kind
# ---------------------------------------------------------------------------


class TestAutocompleteKind:
    def test_requires_option(self, repo_root, make_module):
        make_module("demo", base_manifest())
        before = snapshot(repo_root / "modules")
        result = run_cli(repo_root, "--module", "demo", "--kind", "autocomplete",
                         "--path", "weather")
        assert result.returncode != 0
        assert "option" in (result.stderr + result.stdout).lower()
        assert snapshot(repo_root / "modules") == before

    def test_naming_with_option(self, repo_root, make_module):
        make_module("demo", base_manifest())
        result = run_cli(repo_root, "--module", "demo", "--kind", "autocomplete",
                         "--path", "weather", "--option", "city")
        assert result.returncode == 0, result.stderr
        main_cpp = (repo_root / "modules" / "demo" / "lambdas" / "autocomplete"
                    / "discord-autocomplete-weather-city" / "main.cpp")
        assert main_cpp.exists()
        manifest = read_manifest(repo_root)
        assert (manifest["routes"]["autocomplete"]["weather city"]
                == "discord-autocomplete-weather-city")


# ---------------------------------------------------------------------------
# Refusal on existing route
# ---------------------------------------------------------------------------


class TestRefusal:
    def test_refuses_existing_route(self, repo_root, make_module):
        make_module(
            "demo",
            base_manifest(
                lambdas=["lambdas/commands/discord-cmd-group-sub"],
                routes={"commands": {"group sub": "discord-cmd-group-sub"}},
            ),
            commands=[],
            lambda_mains=["lambdas/commands/discord-cmd-group-sub"],
        )
        before = snapshot(repo_root / "modules")
        result = run_cli(repo_root, "--module", "demo", "--kind", "command",
                         "--path", "group sub")
        assert result.returncode != 0
        assert "exist" in (result.stderr + result.stdout).lower()
        assert snapshot(repo_root / "modules") == before


# ---------------------------------------------------------------------------
# Dry-run writes nothing
# ---------------------------------------------------------------------------


class TestDryRun:
    def test_dry_run_writes_nothing(self, repo_root, make_module):
        make_module("demo", base_manifest(), commands=[])
        before = snapshot(repo_root / "modules")
        result = run_cli(repo_root, "--module", "demo", "--kind", "command",
                         "--path", "group sub", "--dry-run")
        assert result.returncode == 0, result.stderr
        assert snapshot(repo_root / "modules") == before

    def test_dry_run_prints_plan(self, repo_root, make_module):
        make_module("demo", base_manifest(), commands=[])
        result = run_cli(repo_root, "--module", "demo", "--kind", "command",
                         "--path", "group sub", "--dry-run")
        assert "discord-cmd-group-sub" in result.stdout


# ---------------------------------------------------------------------------
# Manifest formatting
# ---------------------------------------------------------------------------


class TestManifestFormatting:
    def test_route_keys_sorted_and_indent_two(self, repo_root, make_module):
        make_module(
            "demo",
            base_manifest(routes={"commands": {"zebra cmd": "discord-cmd-zebra-cmd"}}),
            commands=[],
        )
        run_cli(repo_root, "--module", "demo", "--kind", "command",
                "--path", "alpha beta")
        text = (repo_root / "modules" / "demo" / "module.manifest.json").read_text()
        manifest = json.loads(text)
        assert list(manifest["routes"]["commands"].keys()) == ["alpha beta", "zebra cmd"]
        # 2-space indent: top-level keys are prefixed by exactly two spaces.
        assert '\n  "routes"' in text or '\n  "name"' in text


# ---------------------------------------------------------------------------
# Next-step checklist
# ---------------------------------------------------------------------------


class TestChecklist:
    def test_prints_build_and_register_commands(self, repo_root, make_module):
        make_module("demo", base_manifest(), commands=[])
        result = run_cli(repo_root, "--module", "demo", "--kind", "command",
                         "--path", "group sub")
        assert "build-lambda.sh" in result.stdout
        assert "register-discord-commands.py" in result.stdout


# ---------------------------------------------------------------------------
# Template content — command kind (AGENTS.md rule compliance)
# ---------------------------------------------------------------------------


class TestCommandTemplate:
    def _generate(self, repo_root, make_module):
        make_module("demo", base_manifest(), commands=[])
        run_cli(repo_root, "--module", "demo", "--kind", "command",
                "--path", "group sub")
        return (repo_root / "modules" / "demo" / "lambdas" / "commands"
                / "discord-cmd-group-sub" / "main.cpp").read_text()

    def test_contains_patch_original_response(self, repo_root, make_module):
        assert "patch_original_response" in self._generate(repo_root, make_module)

    def test_contains_module_error_macro(self, repo_root, make_module):
        assert "MODULE_ERROR" in self._generate(repo_root, make_module)

    def test_reads_metadata_via_interaction_header(self, repo_root, make_module):
        text = self._generate(repo_root, make_module)
        assert "discord_interactions/interaction.hpp" in text
        assert "metadata(" in text

    def test_value_initialization_present(self, repo_root, make_module):
        text = self._generate(repo_root, make_module)
        assert "application_id{}" in text

    def test_ephemeral_flag_response(self, repo_root, make_module):
        assert "ephemeral_message" in self._generate(repo_root, make_module)

    def test_never_leaks_what_into_user_payload(self, repo_root, make_module):
        text = self._generate(repo_root, make_module)
        # Every `.what()` use must be a stderr log line, never a PATCH body.
        for line in text.splitlines():
            if "what()" in line:
                assert "stderr" in line, f"what() leaked outside logging: {line!r}"


# ---------------------------------------------------------------------------
# Template content — autocomplete kind (sync {type:8} choices)
# ---------------------------------------------------------------------------


class TestAutocompleteTemplate:
    def _generate(self, repo_root, make_module):
        make_module("demo", base_manifest())
        run_cli(repo_root, "--module", "demo", "--kind", "autocomplete",
                "--path", "weather", "--option", "city")
        return (repo_root / "modules" / "demo" / "lambdas" / "autocomplete"
                / "discord-autocomplete-weather-city" / "main.cpp").read_text()

    def test_returns_autocomplete_response(self, repo_root, make_module):
        assert "autocomplete_response" in self._generate(repo_root, make_module)

    def test_does_not_patch(self, repo_root, make_module):
        assert "patch_original_response" not in self._generate(repo_root, make_module)
