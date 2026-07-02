"""Route <-> schema <-> Lambda-folder consistency checks (T4.2).

These tests drive ``discord_modules.check_route_consistency`` and its wiring
into ``validate_module_manifests`` (the aggregation path used by
``register-discord-commands.py``). The check catches the "naming convention is
load-bearing" failure class mechanically: every command path in a module's
schema must have a manifest route whose target Lambda folder exists and matches
the derived ``discord-cmd-<path>`` name, and vice versa.

Errors fail validation (joined into the ModuleError aggregation); an
unreferenced Lambda folder is surfaced as a WARNING that does not fail.
"""

import pytest

import discord_modules
from discord_modules import (
    ModuleError,
    check_route_consistency,
    discover_modules,
    validate_module_manifests,
)


def run_check(repo_root):
    return check_route_consistency(discover_modules(repo_root))


def base_manifest(name="demo", **extra):
    manifest = {"name": name, "commands": "discord.commands.json"}
    manifest.update(extra)
    return manifest


# ---------------------------------------------------------------------------
# Return shape
# ---------------------------------------------------------------------------


class TestReturnShape:
    def test_no_modules_returns_empty_errors_and_warnings(self, repo_root):
        result = check_route_consistency([])
        assert result == {"errors": [], "warnings": []}

    def test_result_keys_are_lists(self, repo_root, make_module):
        make_module("demo", base_manifest(), commands=[{"name": "x", "description": "d"}])
        result = run_check(repo_root)
        assert isinstance(result["errors"], list)
        assert isinstance(result["warnings"], list)


# ---------------------------------------------------------------------------
# Fully consistent module
# ---------------------------------------------------------------------------


class TestFullyConsistent:
    def test_consistent_module_has_no_errors_or_warnings(self, repo_root, make_module):
        make_module(
            "demo",
            base_manifest(
                lambdas=["lambdas/commands/discord-cmd-demo-hello"],
                routes={"commands": {"demo hello": "discord-cmd-demo-hello"}},
            ),
            commands=[
                {
                    "name": "demo",
                    "description": "d",
                    "options": [{"name": "hello", "description": "h", "type": 1}],
                }
            ],
            lambda_mains=["lambdas/commands/discord-cmd-demo-hello"],
        )
        assert run_check(repo_root) == {"errors": [], "warnings": []}


# ---------------------------------------------------------------------------
# Schema <-> route direction
# ---------------------------------------------------------------------------


class TestSchemaRouteConsistency:
    def test_schema_command_missing_route_is_error(self, repo_root, make_module):
        make_module(
            "demo",
            base_manifest(
                lambdas=["lambdas/commands/discord-cmd-ping"],
                routes={"commands": {"ping": "discord-cmd-ping"}},
            ),
            commands=[
                {"name": "ping", "description": "d"},
                {"name": "pong", "description": "d"},
            ],
            lambda_mains=["lambdas/commands/discord-cmd-ping"],
        )
        result = run_check(repo_root)
        assert result["warnings"] == []
        assert len(result["errors"]) == 1
        (msg,) = result["errors"]
        assert "pong" in msg and "no commands route" in msg

    def test_route_missing_schema_entry_is_error(self, repo_root, make_module):
        make_module(
            "demo",
            base_manifest(
                lambdas=[
                    "lambdas/commands/discord-cmd-ping",
                    "lambdas/commands/discord-cmd-extra-thing",
                ],
                routes={
                    "commands": {
                        "ping": "discord-cmd-ping",
                        "extra thing": "discord-cmd-extra-thing",
                    }
                },
            ),
            commands=[{"name": "ping", "description": "d"}],
            lambda_mains=[
                "lambdas/commands/discord-cmd-ping",
                "lambdas/commands/discord-cmd-extra-thing",
            ],
        )
        result = run_check(repo_root)
        assert len(result["errors"]) == 1
        (msg,) = result["errors"]
        assert "extra thing" in msg and "no matching command" in msg

    def test_route_to_nonexistent_folder_is_error(self, repo_root, make_module):
        # Route names a folder that has no directory / main.cpp on disk.
        make_module(
            "demo",
            base_manifest(routes={"commands": {"ping": "discord-cmd-ping"}}),
            commands=[{"name": "ping", "description": "d"}],
        )
        result = run_check(repo_root)
        assert any(
            "discord-cmd-ping" in m and "does not exist" in m for m in result["errors"]
        )

    def test_folder_name_mismatch_vs_derived_is_warning(self, repo_root, make_module):
        # Fix 5: route-map overrides are a supported feature. A target that
        # differs from the mechanical derivation is a WARNING (ensure it is
        # deployed and IAM-granted), not an error.
        make_module(
            "demo",
            base_manifest(
                lambdas=["lambdas/commands/discord-cmd-pong"],
                routes={"commands": {"ping": "discord-cmd-pong"}},
            ),
            commands=[{"name": "ping", "description": "d"}],
            lambda_mains=["lambdas/commands/discord-cmd-pong"],
        )
        result = run_check(repo_root)
        assert result["errors"] == []
        assert any(
            "discord-cmd-pong" in w and "non-mechanical" in w for w in result["warnings"]
        )


# ---------------------------------------------------------------------------
# Orphan folder -> warning, not error
# ---------------------------------------------------------------------------


class TestOrphanFolder:
    def test_unreferenced_folder_is_warning_not_error(self, repo_root, make_module):
        make_module(
            "demo",
            base_manifest(
                lambdas=[
                    "lambdas/commands/discord-cmd-demo-hello",
                    "lambdas/commands/discord-cmd-orphan",
                ],
                routes={"commands": {"demo hello": "discord-cmd-demo-hello"}},
            ),
            commands=[
                {
                    "name": "demo",
                    "description": "d",
                    "options": [{"name": "hello", "description": "h", "type": 1}],
                }
            ],
            lambda_mains=[
                "lambdas/commands/discord-cmd-demo-hello",
                "lambdas/commands/discord-cmd-orphan",
            ],
        )
        result = run_check(repo_root)
        assert result["errors"] == []
        assert len(result["warnings"]) == 1
        assert "discord-cmd-orphan" in result["warnings"][0]


# ---------------------------------------------------------------------------
# Autocomplete
# ---------------------------------------------------------------------------


def _weather_autocomplete_schema():
    return [
        {
            "name": "weather",
            "description": "d",
            "options": [
                {
                    "name": "city",
                    "description": "d",
                    "type": 3,
                    "autocomplete": True,
                }
            ],
        }
    ]


class TestAutocomplete:
    def test_autocomplete_option_with_matching_route_passes(self, repo_root, make_module):
        make_module(
            "demo",
            base_manifest(
                lambdas=[
                    "lambdas/commands/discord-cmd-weather",
                    "lambdas/autocomplete/discord-autocomplete-weather-city",
                ],
                routes={
                    "commands": {"weather": "discord-cmd-weather"},
                    "autocomplete": {
                        "weather city": "discord-autocomplete-weather-city"
                    },
                },
            ),
            commands=_weather_autocomplete_schema(),
            lambda_mains=[
                "lambdas/commands/discord-cmd-weather",
                "lambdas/autocomplete/discord-autocomplete-weather-city",
            ],
        )
        assert run_check(repo_root) == {"errors": [], "warnings": []}

    def test_autocomplete_option_missing_route_is_error(self, repo_root, make_module):
        make_module(
            "demo",
            base_manifest(
                lambdas=["lambdas/commands/discord-cmd-weather"],
                routes={"commands": {"weather": "discord-cmd-weather"}},
            ),
            commands=_weather_autocomplete_schema(),
            lambda_mains=["lambdas/commands/discord-cmd-weather"],
        )
        result = run_check(repo_root)
        assert any(
            "discord-autocomplete-weather-city" in m and "city" in m
            for m in result["errors"]
        )

    def test_autocomplete_route_present_but_folder_missing_is_error(
        self, repo_root, make_module
    ):
        # Finding 4: the derived autocomplete Lambda name is a route target, but
        # its folder does not exist on disk. This must fail like the command
        # folder checks (mirroring gap fixed).
        make_module(
            "demo",
            base_manifest(
                lambdas=["lambdas/commands/discord-cmd-weather"],
                routes={
                    "commands": {"weather": "discord-cmd-weather"},
                    "autocomplete": {
                        "weather city": "discord-autocomplete-weather-city"
                    },
                },
            ),
            commands=_weather_autocomplete_schema(),
            lambda_mains=["lambdas/commands/discord-cmd-weather"],
        )
        result = run_check(repo_root)
        assert any(
            "discord-autocomplete-weather-city" in m and "does not exist" in m
            for m in result["errors"]
        )


# ---------------------------------------------------------------------------
# Context-menu route kinds (T3.1 vocabulary) - validate if present, tolerate absent
# ---------------------------------------------------------------------------


class TestContextMenuKinds:
    def test_user_commands_present_consistent_passes(self, repo_root, make_module):
        make_module(
            "demo",
            base_manifest(
                lambdas=["lambdas/usercmds/discord-usercmd-report-user"],
                routes={
                    "user_commands": {"Report User": "discord-usercmd-report-user"}
                },
            ),
            commands=[{"name": "Report User", "type": 2}],
            lambda_mains=["lambdas/usercmds/discord-usercmd-report-user"],
        )
        assert run_check(repo_root) == {"errors": [], "warnings": []}

    def test_user_commands_present_mismatch_is_warning(self, repo_root, make_module):
        # Fix 5: a non-mechanical context-menu target is a supported override
        # (WARNING), not an error.
        make_module(
            "demo",
            base_manifest(
                lambdas=["lambdas/usercmds/discord-usercmd-wrong"],
                routes={"user_commands": {"Report User": "discord-usercmd-wrong"}},
            ),
            commands=[{"name": "Report User", "type": 2}],
            lambda_mains=["lambdas/usercmds/discord-usercmd-wrong"],
        )
        result = run_check(repo_root)
        assert result["errors"] == []
        assert any(
            "discord-usercmd-wrong" in w and "non-mechanical" in w
            for w in result["warnings"]
        )

    def test_user_commands_absent_is_tolerated(self, repo_root, make_module):
        # A type-2 command in the schema with no user_commands routes declared
        # must not error (no hard dependency on T3.1 having landed).
        make_module(
            "demo",
            base_manifest(),
            commands=[{"name": "Report User", "type": 2}],
        )
        assert run_check(repo_root) == {"errors": [], "warnings": []}

    def test_message_commands_present_consistent_passes(self, repo_root, make_module):
        make_module(
            "demo",
            base_manifest(
                lambdas=["lambdas/msgcmds/discord-msgcmd-pin-it"],
                routes={"message_commands": {"Pin It": "discord-msgcmd-pin-it"}},
            ),
            commands=[{"name": "Pin It", "type": 3}],
            lambda_mains=["lambdas/msgcmds/discord-msgcmd-pin-it"],
        )
        assert run_check(repo_root) == {"errors": [], "warnings": []}


# ---------------------------------------------------------------------------
# Nested path derivation (group -> subcommand)
# ---------------------------------------------------------------------------


class TestNestedDerivation:
    def test_nested_group_subcommand_path_is_derived(self, repo_root, make_module):
        # cfg (chat) -> user (group, type 2) -> add (subcommand, type 1)
        # derives the path "cfg user add" -> discord-cmd-cfg-user-add.
        make_module(
            "demo",
            base_manifest(
                lambdas=["lambdas/commands/discord-cmd-cfg-user-add"],
                routes={"commands": {"cfg user add": "discord-cmd-cfg-user-add"}},
            ),
            commands=[
                {
                    "name": "cfg",
                    "description": "d",
                    "options": [
                        {
                            "name": "user",
                            "description": "d",
                            "type": 2,
                            "options": [
                                {"name": "add", "description": "d", "type": 1}
                            ],
                        }
                    ],
                }
            ],
            lambda_mains=["lambdas/commands/discord-cmd-cfg-user-add"],
        )
        assert run_check(repo_root) == {"errors": [], "warnings": []}

    def test_nested_path_mismatch_reports_derived_name(self, repo_root, make_module):
        # Fix 5: a non-mechanical override on a nested path is a WARNING that
        # names the mechanical derivation it differs from.
        make_module(
            "demo",
            base_manifest(
                lambdas=["lambdas/commands/discord-cmd-wrong"],
                routes={"commands": {"cfg user add": "discord-cmd-wrong"}},
            ),
            commands=[
                {
                    "name": "cfg",
                    "description": "d",
                    "options": [
                        {
                            "name": "user",
                            "description": "d",
                            "type": 2,
                            "options": [
                                {"name": "add", "description": "d", "type": 1}
                            ],
                        }
                    ],
                }
            ],
            lambda_mains=["lambdas/commands/discord-cmd-wrong"],
        )
        result = run_check(repo_root)
        assert result["errors"] == []
        assert any("discord-cmd-cfg-user-add" in w for w in result["warnings"])


# ---------------------------------------------------------------------------
# Message prefixing + multi-module
# ---------------------------------------------------------------------------


class TestMessagePrefix:
    def test_messages_are_prefixed_with_module_name(self, repo_root, make_module):
        make_module(
            "acme",
            base_manifest(
                name="acme",
                routes={"commands": {"ping": "discord-cmd-ping"}},
            ),
            commands=[{"name": "ping", "description": "d"}],
        )
        result = run_check(repo_root)
        assert result["errors"]
        assert all(m.startswith("acme:") for m in result["errors"])


# ---------------------------------------------------------------------------
# Route-override policy (Fix 5): non-mechanical command targets are warnings;
# components/modals gain symmetric mechanical-target folder checks.
# ---------------------------------------------------------------------------


class TestCommandOverridePolicy:
    def test_non_mechanical_command_target_is_warning(self, repo_root, make_module):
        make_module(
            "demo",
            base_manifest(
                lambdas=["lambdas/commands/discord-cmd-shared"],
                routes={"commands": {"ping": "discord-cmd-shared"}},
            ),
            commands=[{"name": "ping", "description": "d"}],
            lambda_mains=["lambdas/commands/discord-cmd-shared"],
        )
        result = run_check(repo_root)
        assert result["errors"] == []
        assert any(
            "non-mechanical" in w and "IAM" in w for w in result["warnings"]
        )

    def test_mechanical_command_target_missing_folder_still_errors(
        self, repo_root, make_module
    ):
        make_module(
            "demo",
            base_manifest(routes={"commands": {"ping": "discord-cmd-ping"}}),
            commands=[{"name": "ping", "description": "d"}],
        )
        result = run_check(repo_root)
        assert any(
            "discord-cmd-ping" in m and "does not exist" in m for m in result["errors"]
        )


class TestComponentModalConsistency:
    def test_component_mechanical_target_missing_folder_is_error(
        self, repo_root, make_module
    ):
        make_module(
            "demo",
            base_manifest(routes={"components": {"vote": "discord-component-vote"}}),
        )
        result = run_check(repo_root)
        assert any(
            "discord-component-vote" in m and "does not exist" in m
            for m in result["errors"]
        )

    def test_component_mechanical_target_present_passes(self, repo_root, make_module):
        make_module(
            "demo",
            base_manifest(
                lambdas=["lambdas/components/discord-component-vote"],
                routes={"components": {"vote": "discord-component-vote"}},
            ),
            lambda_mains=["lambdas/components/discord-component-vote"],
        )
        assert run_check(repo_root)["errors"] == []

    def test_component_non_mechanical_target_is_warning(self, repo_root, make_module):
        make_module(
            "demo",
            base_manifest(
                routes={"components": {"vote": "discord-component-shared-handler"}}
            ),
        )
        result = run_check(repo_root)
        assert result["errors"] == []
        assert any(
            "discord-component-shared-handler" in w and "non-mechanical" in w
            for w in result["warnings"]
        )

    def test_modal_mechanical_target_missing_folder_is_error(
        self, repo_root, make_module
    ):
        make_module(
            "demo",
            base_manifest(routes={"modals": {"feedback": "discord-modal-feedback"}}),
        )
        result = run_check(repo_root)
        assert any(
            "discord-modal-feedback" in m and "does not exist" in m
            for m in result["errors"]
        )


# ---------------------------------------------------------------------------
# Wiring into validate_module_manifests
# ---------------------------------------------------------------------------


class TestWiring:
    def test_inconsistent_module_fails_manifest_validation(self, repo_root, make_module):
        # A mechanical target whose folder is missing is still a hard error
        # (Fix 5 only relaxes non-mechanical overrides to warnings).
        make_module(
            "demo",
            base_manifest(routes={"commands": {"ping": "discord-cmd-ping"}}),
            commands=[{"name": "ping", "description": "d"}],
        )
        with pytest.raises(ModuleError, match="does not exist"):
            validate_module_manifests(repo_root)

    def test_orphan_warning_does_not_fail_validation(self, repo_root, make_module, capsys):
        make_module(
            "demo",
            base_manifest(
                lambdas=[
                    "lambdas/commands/discord-cmd-demo-hello",
                    "lambdas/commands/discord-cmd-orphan",
                ],
                routes={"commands": {"demo hello": "discord-cmd-demo-hello"}},
            ),
            commands=[
                {
                    "name": "demo",
                    "description": "d",
                    "options": [{"name": "hello", "description": "h", "type": 1}],
                }
            ],
            lambda_mains=[
                "lambdas/commands/discord-cmd-demo-hello",
                "lambdas/commands/discord-cmd-orphan",
            ],
        )
        validate_module_manifests(repo_root)  # must not raise
        err = capsys.readouterr().err
        assert "discord-cmd-orphan" in err
        assert "not referenced by any route" in err
