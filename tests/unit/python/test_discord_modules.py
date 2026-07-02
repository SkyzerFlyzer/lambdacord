"""Characterization tests for scripts/lib/discord_modules.py.

These pin down the current behavior of manifest discovery, command/schema
merging, route merging (including duplicate rejection), and manifest
validation so later tooling tasks can TDD against this layer.
"""

import pytest

import discord_modules
from discord_modules import (
    ModuleError,
    all_module_commands,
    discover_modules,
    lambda_dirs,
    module_commands,
    read_json,
    route_map,
    validate_module_manifests,
)


def minimal_manifest(name):
    return {"name": name, "commands": "discord.commands.json"}


# ---------------------------------------------------------------------------
# read_json
# ---------------------------------------------------------------------------


class TestReadJson:
    def test_returns_parsed_json(self, tmp_path):
        path = tmp_path / "data.json"
        path.write_text('{"a": [1, 2]}', encoding="utf-8")
        assert read_json(path) == {"a": [1, 2]}

    def test_missing_file_raises_module_error_with_path(self, tmp_path):
        path = tmp_path / "nope.json"
        with pytest.raises(ModuleError, match="module file not found"):
            read_json(path)

    def test_invalid_json_raises_module_error_with_path(self, tmp_path):
        path = tmp_path / "bad.json"
        path.write_text("{not json", encoding="utf-8")
        with pytest.raises(ModuleError, match="invalid JSON in"):
            read_json(path)


# ---------------------------------------------------------------------------
# discover_modules
# ---------------------------------------------------------------------------


class TestDiscoverModules:
    def test_returns_empty_list_when_modules_dir_absent(self, repo_root):
        assert discover_modules(repo_root) == []

    def test_returns_dir_manifest_path_and_manifest(self, repo_root, make_module):
        module_dir = make_module("demo", minimal_manifest("demo"))
        modules = discover_modules(repo_root)
        assert len(modules) == 1
        assert modules[0]["dir"] == module_dir
        assert modules[0]["manifest_path"] == module_dir / "module.manifest.json"
        assert modules[0]["manifest"]["name"] == "demo"

    def test_modules_discovered_in_sorted_directory_order(self, repo_root, make_module):
        make_module("zeta", minimal_manifest("zeta"))
        make_module("alpha", minimal_manifest("alpha"))
        names = [m["manifest"]["name"] for m in discover_modules(repo_root)]
        assert names == ["alpha", "zeta"]

    def test_directory_without_manifest_is_ignored(self, repo_root, make_module):
        make_module("demo", minimal_manifest("demo"))
        (repo_root / "modules" / "not-a-module").mkdir()
        names = [m["manifest"]["name"] for m in discover_modules(repo_root)]
        assert names == ["demo"]

    def test_manifest_missing_name_is_rejected(self, repo_root, make_module):
        make_module("demo", {"commands": "discord.commands.json"})
        with pytest.raises(ModuleError, match="non-empty string name"):
            discover_modules(repo_root)

    def test_manifest_non_string_name_is_rejected(self, repo_root, make_module):
        make_module("demo", {"name": 42})
        with pytest.raises(ModuleError, match="non-empty string name"):
            discover_modules(repo_root)

    def test_manifest_with_malformed_json_is_rejected(self, repo_root, make_module):
        make_module("demo", '{"name": "demo",}')
        with pytest.raises(ModuleError, match="invalid JSON in"):
            discover_modules(repo_root)


# ---------------------------------------------------------------------------
# module_commands / all_module_commands (schema merging)
# ---------------------------------------------------------------------------


class TestModuleCommands:
    def test_reads_command_array_from_declared_file(self, repo_root, make_module):
        make_module(
            "demo",
            {"name": "demo", "commands": "schemas/cmds.json"},
            commands=[{"name": "demo", "description": "d"}],
        )
        (module,) = discover_modules(repo_root)
        assert module_commands(module) == [{"name": "demo", "description": "d"}]

    def test_manifest_without_commands_file_is_rejected(self, repo_root, make_module):
        make_module("demo", {"name": "demo"})
        (module,) = discover_modules(repo_root)
        with pytest.raises(ModuleError, match="must include a commands file"):
            module_commands(module)

    def test_non_array_commands_file_is_rejected(self, repo_root, make_module):
        make_module("demo", minimal_manifest("demo"), commands={"name": "demo"})
        (module,) = discover_modules(repo_root)
        with pytest.raises(ModuleError, match="must contain a JSON array"):
            module_commands(module)

    def test_missing_commands_file_on_disk_is_rejected(self, repo_root, make_module):
        make_module("demo", minimal_manifest("demo"))  # commands file never written
        (module,) = discover_modules(repo_root)
        with pytest.raises(ModuleError, match="module file not found"):
            module_commands(module)

    def test_all_module_commands_merges_in_module_order(self, repo_root, make_module):
        make_module(
            "beta", minimal_manifest("beta"), commands=[{"name": "b", "description": "b"}]
        )
        make_module(
            "alpha",
            minimal_manifest("alpha"),
            commands=[{"name": "a1", "description": "a"}, {"name": "a2", "description": "a"}],
        )
        merged = all_module_commands(repo_root)
        assert [c["name"] for c in merged] == ["a1", "a2", "b"]

    def test_all_module_commands_empty_without_modules(self, repo_root):
        assert all_module_commands(repo_root) == []


# ---------------------------------------------------------------------------
# lambda_dirs
# ---------------------------------------------------------------------------


class TestLambdaDirs:
    CORE = [
        "discord-interactions",
        "discord-application-command-handler",
        "discord-message-component-handler",
        "discord-modal-handler",
        "discord-autocomplete-handler",
    ]

    def test_core_framework_lambdas_always_listed(self, repo_root):
        dirs = lambda_dirs(repo_root)
        assert dirs == [repo_root / "src" / "lambdas" / name for name in self.CORE]

    def test_module_lambda_dirs_appended_after_core(self, repo_root, make_module):
        manifest = dict(minimal_manifest("demo"))
        manifest["lambdas"] = ["lambdas/commands/discord-cmd-demo-hello"]
        module_dir = make_module("demo", manifest)
        dirs = lambda_dirs(repo_root)
        assert dirs[:5] == [repo_root / "src" / "lambdas" / name for name in self.CORE]
        assert dirs[5:] == [module_dir / "lambdas/commands/discord-cmd-demo-hello"]

    def test_invalid_lambda_path_entry_is_rejected(self, repo_root, make_module):
        manifest = dict(minimal_manifest("demo"))
        manifest["lambdas"] = [""]
        make_module("demo", manifest)
        with pytest.raises(ModuleError, match="invalid lambda path"):
            lambda_dirs(repo_root)


# ---------------------------------------------------------------------------
# route_map (route merging + duplicate rejection)
# ---------------------------------------------------------------------------


class TestRouteMap:
    def test_empty_when_no_modules(self, repo_root):
        assert route_map(repo_root, "commands") == {}

    def test_empty_when_route_kind_absent(self, repo_root, make_module):
        make_module("demo", minimal_manifest("demo"))
        assert route_map(repo_root, "components") == {}

    def test_merges_routes_across_modules(self, repo_root, make_module):
        manifest_a = dict(minimal_manifest("alpha"))
        manifest_a["routes"] = {"commands": {"alpha hello": "discord-cmd-alpha-hello"}}
        make_module("alpha", manifest_a)
        manifest_b = dict(minimal_manifest("beta"))
        manifest_b["routes"] = {"commands": {"beta hello": "discord-cmd-beta-hello"}}
        make_module("beta", manifest_b)
        assert route_map(repo_root, "commands") == {
            "alpha hello": "discord-cmd-alpha-hello",
            "beta hello": "discord-cmd-beta-hello",
        }

    def test_route_kinds_are_independent(self, repo_root, make_module):
        manifest = dict(minimal_manifest("demo"))
        manifest["routes"] = {
            "commands": {"demo hello": "discord-cmd-demo-hello"},
            "components": {"demo": "discord-component-demo"},
        }
        make_module("demo", manifest)
        assert route_map(repo_root, "commands") == {"demo hello": "discord-cmd-demo-hello"}
        assert route_map(repo_root, "components") == {"demo": "discord-component-demo"}

    def test_duplicate_route_across_modules_is_rejected(self, repo_root, make_module):
        for name in ("alpha", "beta"):
            manifest = dict(minimal_manifest(name))
            manifest["routes"] = {"commands": {"demo hello": f"discord-cmd-{name}"}}
            make_module(name, manifest)
        with pytest.raises(ModuleError, match="duplicate commands route: demo hello"):
            route_map(repo_root, "commands")

    def test_non_object_route_kind_is_rejected(self, repo_root, make_module):
        manifest = dict(minimal_manifest("demo"))
        manifest["routes"] = {"commands": ["demo hello"]}
        make_module("demo", manifest)
        with pytest.raises(ModuleError, match=r"routes\.commands must be an object"):
            route_map(repo_root, "commands")


# ---------------------------------------------------------------------------
# validate_module_manifests
# ---------------------------------------------------------------------------


def full_valid_manifest():
    return {
        "name": "demo",
        "commands": "discord.commands.json",
        "lambdas": ["lambdas/commands/discord-cmd-demo-hello"],
        "routes": {"commands": {"demo hello": "discord-cmd-demo-hello"}},
        "errors": "discord.errors.json",
        "error_mapper": {"path": "lambdas/errors/discord-error-mapper-demo"},
        "terraform": "terraform",
    }


def make_full_valid_module(make_module):
    return make_module(
        "demo",
        full_valid_manifest(),
        commands=[{"name": "demo", "description": "d"}],
        errors={"errors": {"some_code": {"message": "friendly copy"}}},
        lambda_mains=[
            "lambdas/commands/discord-cmd-demo-hello",
            "lambdas/errors/discord-error-mapper-demo",
        ],
        extra_files=[("terraform/main.tf", "# test fixture\n")],
    )


class TestValidateModuleManifests:
    def test_valid_module_passes(self, repo_root, make_module):
        make_full_valid_module(make_module)
        validate_module_manifests(repo_root)  # must not raise

    def test_no_modules_passes(self, repo_root):
        validate_module_manifests(repo_root)  # must not raise

    def test_lambda_without_main_cpp_reported(self, repo_root, make_module):
        manifest = dict(minimal_manifest("demo"))
        manifest["lambdas"] = ["lambdas/commands/discord-cmd-demo-hello"]
        make_module("demo", manifest)
        with pytest.raises(ModuleError, match="demo lambda missing main.cpp"):
            validate_module_manifests(repo_root)

    def test_non_object_route_kind_reported(self, repo_root, make_module):
        manifest = dict(minimal_manifest("demo"))
        manifest["routes"] = {"modals": ["oops"]}
        make_module("demo", manifest)
        with pytest.raises(ModuleError, match=r"routes\.modals must be an object"):
            validate_module_manifests(repo_root)

    def test_non_string_function_name_reported(self, repo_root, make_module):
        manifest = dict(minimal_manifest("demo"))
        manifest["routes"] = {"commands": {"demo hello": 123}}
        make_module("demo", manifest)
        with pytest.raises(
            ModuleError, match="route 'demo hello' has an invalid function name"
        ):
            validate_module_manifests(repo_root)

    def test_empty_route_key_reported(self, repo_root, make_module):
        manifest = dict(minimal_manifest("demo"))
        manifest["routes"] = {"commands": {"": "discord-cmd-x"}}
        make_module("demo", manifest)
        with pytest.raises(ModuleError, match="has an invalid commands route"):
            validate_module_manifests(repo_root)

    def test_error_mapper_without_main_cpp_reported(self, repo_root, make_module):
        manifest = dict(minimal_manifest("demo"))
        manifest["error_mapper"] = {"path": "lambdas/errors/discord-error-mapper-demo"}
        make_module("demo", manifest)
        with pytest.raises(ModuleError, match="error mapper missing main.cpp"):
            validate_module_manifests(repo_root)

    def test_errors_file_without_errors_object_reported(self, repo_root, make_module):
        manifest = dict(minimal_manifest("demo"))
        manifest["errors"] = "discord.errors.json"
        make_module("demo", manifest, errors={"errors": ["not", "an", "object"]})
        with pytest.raises(ModuleError, match="must contain an errors object"):
            validate_module_manifests(repo_root)

    def test_terraform_without_main_tf_reported(self, repo_root, make_module):
        manifest = dict(minimal_manifest("demo"))
        manifest["terraform"] = "terraform"
        make_module("demo", manifest)
        with pytest.raises(ModuleError, match="terraform missing main.tf"):
            validate_module_manifests(repo_root)

    def test_multiple_problems_aggregated_with_semicolons(self, repo_root, make_module):
        manifest = dict(minimal_manifest("demo"))
        manifest["lambdas"] = ["lambdas/commands/discord-cmd-demo-hello"]
        manifest["terraform"] = "terraform"
        make_module("demo", manifest)
        with pytest.raises(ModuleError) as excinfo:
            validate_module_manifests(repo_root)
        message = str(excinfo.value)
        assert "lambda missing main.cpp" in message
        assert "terraform missing main.tf" in message
        assert "; " in message

    def test_validation_does_not_check_commands_file_contents(
        self, repo_root, make_module
    ):
        # Characterization: validate_module_manifests never opens the commands
        # schema file; only module_commands()/registration does.
        make_module("demo", minimal_manifest("demo"))  # commands file absent on disk
        validate_module_manifests(repo_root)  # must not raise
