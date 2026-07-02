"""Manifest-driven ephemeral defer (T3.2) — Python layer.

Covers the object route form in ``scripts/lib/discord_modules.py`` (parsing,
validation, the ``ephemeral_defer_routes`` merged-list helper) and the
``DISCORD_EPHEMERAL_DEFER_ROUTES`` emission in
``scripts/generate-terraform-modules.py``.

All module fixtures live in pytest tmp dirs (``make_module`` from conftest).
The generator is exercised through its ``generate(repo_root)`` function, which
already takes the repository root as a parameter — no repo-scanning override
mechanism is needed for testability.
"""

import importlib.util
from pathlib import Path

import pytest

from discord_modules import (
    ModuleError,
    ephemeral_defer_routes,
    route_map,
    validate_module_manifests,
)

REPO_ROOT = Path(__file__).resolve().parents[3]
GENERATOR = REPO_ROOT / "scripts" / "generate-terraform-modules.py"


def load_generator():
    spec = importlib.util.spec_from_file_location("generate_terraform_modules", GENERATOR)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# ---------------------------------------------------------------------------
# route_map: object route form
# ---------------------------------------------------------------------------


def test_route_map_normalizes_object_form_to_lambda_name(repo_root, make_module):
    make_module(
        "demo",
        {
            "name": "demo",
            "routes": {
                "commands": {
                    "account link": {
                        "lambda": "discord-cmd-account-link",
                        "ephemeral_defer": True,
                    }
                }
            },
        },
    )
    assert route_map(repo_root, "commands") == {
        "account link": "discord-cmd-account-link"
    }


def test_route_map_mixes_string_and_object_forms(repo_root, make_module):
    make_module(
        "demo",
        {
            "name": "demo",
            "routes": {
                "commands": {
                    "account link": {
                        "lambda": "discord-cmd-account-link",
                        "ephemeral_defer": True,
                    },
                    "account list": "discord-cmd-account-list",
                }
            },
        },
    )
    assert route_map(repo_root, "commands") == {
        "account link": "discord-cmd-account-link",
        "account list": "discord-cmd-account-list",
    }


# ---------------------------------------------------------------------------
# ephemeral_defer_routes: merged, sorted, deduplicated
# ---------------------------------------------------------------------------


def test_ephemeral_defer_routes_merges_sorted_and_deduped():
    manifests = [
        {
            "name": "m1",
            "routes": {
                "commands": {
                    "example ping": {"lambda": "discord-cmd-example-ping", "ephemeral_defer": True},
                    "zeta cmd": {"lambda": "discord-cmd-zeta-cmd", "ephemeral_defer": True},
                }
            },
        },
        {
            "name": "m2",
            "routes": {
                "commands": {
                    # Duplicate of m1's entry — must be deduplicated.
                    "example ping": {"lambda": "discord-cmd-example-ping", "ephemeral_defer": True},
                    "account link": {"lambda": "discord-cmd-account-link", "ephemeral_defer": True},
                }
            },
        },
    ]
    assert ephemeral_defer_routes(manifests) == [
        "account link",
        "example ping",
        "zeta cmd",
    ]


def test_ephemeral_defer_routes_string_and_false_forms_contribute_nothing():
    manifests = [
        {
            "name": "m1",
            "routes": {
                "commands": {
                    "plain string": "discord-cmd-plain-string",
                    "explicit false": {
                        "lambda": "discord-cmd-explicit-false",
                        "ephemeral_defer": False,
                    },
                    "no flag": {"lambda": "discord-cmd-no-flag"},
                },
                # Only command routes may opt in; other kinds never contribute.
                "components": {"pager": "discord-component-pager"},
            },
        }
    ]
    assert ephemeral_defer_routes(manifests) == []


def test_ephemeral_defer_routes_empty_manifest_list():
    assert ephemeral_defer_routes([]) == []


def test_ephemeral_defer_routes_collects_context_menu_kinds():
    """Finding 2: user_commands / message_commands may opt in, keyed by the raw
    command name (the ingress matches context menus on the raw name)."""
    manifests = [
        {
            "name": "m1",
            "routes": {
                "commands": {
                    "account link": {
                        "lambda": "discord-cmd-account-link",
                        "ephemeral_defer": True,
                    }
                },
                "user_commands": {
                    "Report User": {
                        "lambda": "discord-usercmd-report-user",
                        "ephemeral_defer": True,
                    },
                    "Block User": {"lambda": "discord-usercmd-block-user"},
                },
                "message_commands": {
                    "Pin Message": {
                        "lambda": "discord-msgcmd-pin-message",
                        "ephemeral_defer": True,
                    }
                },
            },
        }
    ]
    assert ephemeral_defer_routes(manifests) == [
        "Pin Message",
        "Report User",
        "account link",
    ]


# ---------------------------------------------------------------------------
# Validation of the object route form
# ---------------------------------------------------------------------------


def test_validation_accepts_object_form_command_route(repo_root, make_module):
    make_module(
        "demo",
        {
            "name": "demo",
            "routes": {
                "commands": {
                    "account link": {
                        "lambda": "discord-cmd-account-link",
                        "ephemeral_defer": True,
                    }
                }
            },
        },
    )
    validate_module_manifests(repo_root)  # must not raise


def test_validation_rejects_ephemeral_defer_on_component_routes(repo_root, make_module):
    make_module(
        "demo",
        {
            "name": "demo",
            "routes": {
                "components": {
                    "pager": {
                        "lambda": "discord-component-pager",
                        "ephemeral_defer": True,
                    }
                }
            },
        },
    )
    with pytest.raises(ModuleError, match="ephemeral_defer"):
        validate_module_manifests(repo_root)


def test_validation_accepts_ephemeral_defer_on_user_commands(repo_root, make_module):
    # Finding 2: context-menu route kinds may carry the flag.
    make_module(
        "demo",
        {
            "name": "demo",
            "routes": {
                "user_commands": {
                    "Report User": {
                        "lambda": "discord-usercmd-report-user",
                        "ephemeral_defer": True,
                    }
                }
            },
        },
    )
    validate_module_manifests(repo_root)  # must not raise


def test_validation_accepts_ephemeral_defer_on_message_commands(repo_root, make_module):
    make_module(
        "demo",
        {
            "name": "demo",
            "routes": {
                "message_commands": {
                    "Pin Message": {
                        "lambda": "discord-msgcmd-pin-message",
                        "ephemeral_defer": True,
                    }
                }
            },
        },
    )
    validate_module_manifests(repo_root)  # must not raise


def test_validation_rejects_non_boolean_ephemeral_defer(repo_root, make_module):
    make_module(
        "demo",
        {
            "name": "demo",
            "routes": {
                "commands": {
                    "account link": {
                        "lambda": "discord-cmd-account-link",
                        "ephemeral_defer": "yes",
                    }
                }
            },
        },
    )
    with pytest.raises(ModuleError, match="ephemeral_defer.*boolean"):
        validate_module_manifests(repo_root)


def test_validation_rejects_object_route_missing_lambda(repo_root, make_module):
    make_module(
        "demo",
        {
            "name": "demo",
            "routes": {"commands": {"account link": {"ephemeral_defer": True}}},
        },
    )
    with pytest.raises(ModuleError, match="lambda"):
        validate_module_manifests(repo_root)


def test_validation_still_rejects_non_string_non_object_route_values(repo_root, make_module):
    make_module(
        "demo",
        {"name": "demo", "routes": {"commands": {"account link": 42}}},
    )
    with pytest.raises(ModuleError):
        validate_module_manifests(repo_root)


# ---------------------------------------------------------------------------
# Generator: DISCORD_EPHEMERAL_DEFER_ROUTES emission in generated Terraform
# ---------------------------------------------------------------------------


def test_generator_emits_merged_csv_local(repo_root, make_module):
    make_module(
        "m1",
        {
            "name": "m1",
            "routes": {
                "commands": {
                    "example ping": {
                        "lambda": "discord-cmd-example-ping",
                        "ephemeral_defer": True,
                    }
                }
            },
        },
    )
    make_module(
        "m2",
        {
            "name": "m2",
            "routes": {
                "commands": {
                    "account link": {
                        "lambda": "discord-cmd-account-link",
                        "ephemeral_defer": True,
                    },
                    "account list": "discord-cmd-account-list",
                }
            },
        },
    )
    generator = load_generator()
    files = generator.generate(repo_root)
    modules_tf = files[repo_root / "infra" / "terraform" / "generated_modules.tf"]
    assert 'discord_ephemeral_defer_routes = "account link,example ping"' in modules_tf


def test_generator_emits_context_menu_raw_names_in_csv(repo_root, make_module):
    # Finding 2: opted-in context-menu commands (matched by raw name) merge into
    # the generated DISCORD_EPHEMERAL_DEFER_ROUTES list alongside command paths.
    make_module(
        "m1",
        {
            "name": "m1",
            "routes": {
                "commands": {
                    "account link": {
                        "lambda": "discord-cmd-account-link",
                        "ephemeral_defer": True,
                    }
                },
                "user_commands": {
                    "Report User": {
                        "lambda": "discord-usercmd-report-user",
                        "ephemeral_defer": True,
                    }
                },
            },
        },
    )
    generator = load_generator()
    files = generator.generate(repo_root)
    modules_tf = files[repo_root / "infra" / "terraform" / "generated_modules.tf"]
    assert 'discord_ephemeral_defer_routes = "Report User,account link"' in modules_tf


def test_generator_empty_case_emits_no_stale_value(repo_root, make_module):
    make_module(
        "m1",
        {
            "name": "m1",
            "routes": {"commands": {"example ping": "discord-cmd-example-ping"}},
        },
    )
    generator = load_generator()
    files = generator.generate(repo_root)
    modules_tf = files[repo_root / "infra" / "terraform" / "generated_modules.tf"]
    # The local must exist (the root main.tf references it unconditionally) but
    # carry an empty value — never a stale route list.
    assert 'discord_ephemeral_defer_routes = ""' in modules_tf
    assert "example ping" not in modules_tf.split("discord_ephemeral_defer_routes")[1].split("\n")[0]


def test_root_main_tf_wires_generated_local_into_ingress_env():
    main_tf = (REPO_ROOT / "infra" / "terraform" / "main.tf").read_text(encoding="utf-8")
    assert "DISCORD_EPHEMERAL_DEFER_ROUTES" in main_tf
    assert "local.discord_ephemeral_defer_routes" in main_tf
