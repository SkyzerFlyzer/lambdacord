"""Route-kind vocabulary tests for context menu commands (T3.1).

The type-2/3 schema *shape* rules are owned by T4.1 and are not re-implemented
here; one end-to-end case below proves those rules fire for a type-2 schema
entry. Everything else asserts routing/vocabulary behavior only.
"""

import pytest

from discord_modules import (
    ModuleError,
    check_route_consistency,
    discover_modules,
    route_map,
    validate_module_manifests,
)


def test_route_map_merges_user_command_routes(repo_root, make_module):
    make_module(
        "alpha",
        {
            "name": "alpha",
            "routes": {"user_commands": {"Report User": "discord-usercmd-report-user"}},
        },
    )
    make_module(
        "beta",
        {
            "name": "beta",
            "routes": {"user_commands": {"Block User": "discord-usercmd-block-user"}},
        },
    )

    assert route_map(repo_root, "user_commands") == {
        "Report User": "discord-usercmd-report-user",
        "Block User": "discord-usercmd-block-user",
    }


def test_route_map_merges_message_command_routes(repo_root, make_module):
    make_module(
        "alpha",
        {
            "name": "alpha",
            "routes": {"message_commands": {"Pin Message": "discord-msgcmd-pin-message"}},
        },
    )

    assert route_map(repo_root, "message_commands") == {
        "Pin Message": "discord-msgcmd-pin-message"
    }


def test_route_map_rejects_duplicate_user_command_routes(repo_root, make_module):
    make_module(
        "alpha",
        {
            "name": "alpha",
            "routes": {"user_commands": {"Report User": "discord-usercmd-report-user"}},
        },
    )
    make_module(
        "beta",
        {
            "name": "beta",
            "routes": {"user_commands": {"Report User": "discord-usercmd-other"}},
        },
    )

    with pytest.raises(ModuleError, match="duplicate user_commands route"):
        route_map(repo_root, "user_commands")


def test_validation_accepts_context_menu_route_kinds(repo_root, make_module):
    make_module(
        "alpha",
        {
            "name": "alpha",
            "commands": "discord.commands.json",
            "lambdas": [
                "lambdas/usercmds/discord-usercmd-report-user",
                "lambdas/msgcmds/discord-msgcmd-pin-message",
            ],
            "routes": {
                "user_commands": {"Report User": "discord-usercmd-report-user"},
                "message_commands": {"Pin Message": "discord-msgcmd-pin-message"},
            },
        },
        commands=[
            {"name": "Report User", "type": 2},
            {"name": "Pin Message", "type": 3},
        ],
        lambda_mains=(
            "lambdas/usercmds/discord-usercmd-report-user",
            "lambdas/msgcmds/discord-msgcmd-pin-message",
        ),
    )

    validate_module_manifests(repo_root)  # must not raise


def test_validation_rejects_non_object_user_commands_route_map(repo_root, make_module):
    make_module(
        "alpha",
        {"name": "alpha", "routes": {"user_commands": ["Report User"]}},
    )

    with pytest.raises(ModuleError, match=r"routes\.user_commands must be an object"):
        validate_module_manifests(repo_root)


def test_validation_rejects_invalid_message_command_function_name(repo_root, make_module):
    make_module(
        "alpha",
        {"name": "alpha", "routes": {"message_commands": {"Pin Message": 7}}},
    )

    with pytest.raises(ModuleError, match="invalid function name"):
        validate_module_manifests(repo_root)


def test_consistency_check_tolerates_context_menu_kinds(repo_root, make_module):
    """T4.2's consistency check must handle the new kinds without extension.

    A correct schema/route/folder triple for both kinds produces no errors,
    and the routed folders are counted as referenced (no orphan warnings).
    """
    make_module(
        "alpha",
        {
            "name": "alpha",
            "commands": "discord.commands.json",
            "lambdas": [
                "lambdas/usercmds/discord-usercmd-report-user",
                "lambdas/msgcmds/discord-msgcmd-pin-message",
            ],
            "routes": {
                "user_commands": {"Report User": "discord-usercmd-report-user"},
                "message_commands": {"Pin Message": "discord-msgcmd-pin-message"},
            },
        },
        commands=[
            {"name": "Report User", "type": 2},
            {"name": "Pin Message", "type": 3},
        ],
        lambda_mains=(
            "lambdas/usercmds/discord-usercmd-report-user",
            "lambdas/msgcmds/discord-msgcmd-pin-message",
        ),
    )

    result = check_route_consistency(discover_modules(repo_root))
    assert result["errors"] == []
    assert result["warnings"] == []


def test_consistency_check_flags_misnamed_user_command_target(repo_root, make_module):
    make_module(
        "alpha",
        {
            "name": "alpha",
            "commands": "discord.commands.json",
            "lambdas": ["lambdas/usercmds/discord-usercmd-wrong"],
            "routes": {"user_commands": {"Report User": "discord-usercmd-wrong"}},
        },
        commands=[{"name": "Report User", "type": 2}],
        lambda_mains=("lambdas/usercmds/discord-usercmd-wrong",),
    )

    result = check_route_consistency(discover_modules(repo_root))
    assert any(
        "derived Lambda name is 'discord-usercmd-report-user'" in error
        for error in result["errors"]
    )


def test_consistency_check_uses_ascii_only_lowercasing_for_context_menu(
    repo_root, make_module
):
    """Finding 1: the Python derivation must be byte-identical to the C++ router.

    ``context_menu_route_suffix`` (src/include/discord_interactions/interaction.hpp)
    lowercases ASCII bytes only and leaves multibyte UTF-8 untouched, so
    "Über User" -> "Über-user". Python's ``str.lower()`` would fold the non-ASCII
    Ü to "über-user", deriving a folder the router never invokes. The consistency
    check must expect the folder the C++ router actually targets.
    """
    make_module(
        "alpha",
        {
            "name": "alpha",
            "commands": "discord.commands.json",
            "lambdas": ["lambdas/usercmds/discord-usercmd-Über-user"],
            "routes": {
                "user_commands": {"Über User": "discord-usercmd-Über-user"}
            },
        },
        commands=[{"name": "Über User", "type": 2}],
        lambda_mains=("lambdas/usercmds/discord-usercmd-Über-user",),
    )

    result = check_route_consistency(discover_modules(repo_root))
    assert result["errors"] == []
    assert result["warnings"] == []


def test_t4_1_shape_rules_fire_for_type_2_schema_entry(repo_root, make_module):
    """End-to-end: T4.1's context-menu shape rules run inside full validation."""
    make_module(
        "alpha",
        {
            "name": "alpha",
            "commands": "discord.commands.json",
            "lambdas": ["lambdas/usercmds/discord-usercmd-report-user"],
            "routes": {"user_commands": {"Report User": "discord-usercmd-report-user"}},
        },
        commands=[
            {
                "name": "Report User",
                "type": 2,
                "description": "context menu commands must not have one",
            }
        ],
        lambda_mains=("lambdas/usercmds/discord-usercmd-report-user",),
    )

    with pytest.raises(ModuleError, match="must not have a description"):
        validate_module_manifests(repo_root)
