#!/usr/bin/env python3
"""Scaffolding generator for module Lambda skeletons (T4.3).

``create-discord-bot``-style DX: generate a correctly named
command/component/modal/autocomplete Lambda inside a module, wire its
``module.manifest.json`` route, append a schema stub to
``discord.commands.json`` (command kind only), and print a next-step checklist.

The generated ``main.cpp`` already follows every AGENTS.md/CLAUDE.md rule:
value-initialize with ``{}``, read ``application_id``/``token`` via
``discord_interactions::metadata`` (``discord_interactions/interaction.hpp``),
PATCH the deferred response via ``discord_interactions::patch_original_response``
on both the success and safe-error paths, raise ``MODULE_ERROR`` for expected
failures, log internals to stderr only, and never leak ``ex.what()`` into
user-facing copy. Autocomplete sits on the sync path and instead returns the
``{type:8}`` choice payload via ``discord_interactions::autocomplete_response``.

Usage:
    python3 scripts/new-lambda.py --module <name> --kind command|component|modal|autocomplete \\
        --path "group sub" [--option <name>] [--dry-run] [--modules-root <dir>]

Manifest parsing is reused from ``scripts/lib/discord_modules.py`` (AD-6); this
script never duplicates it.
"""

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
from discord_modules import ModuleError, read_json  # noqa: E402


# Maps a CLI kind to (route-kind key in manifest, lambdas/<subdir>, name prefix).
KIND_SPEC = {
    "command": ("commands", "commands", "discord-cmd-"),
    "component": ("components", "components", "discord-component-"),
    "modal": ("modals", "modals", "discord-modal-"),
    "autocomplete": ("autocomplete", "autocomplete", "discord-autocomplete-"),
}


# ---------------------------------------------------------------------------
# Derivation helpers
# ---------------------------------------------------------------------------


def derive(kind, path_tokens, option):
    """Return (lambda_name, route_key) for a kind + parsed path + option.

    - command:      "group sub"       -> discord-cmd-group-sub,        key "group sub"
    - component:    "vote"            -> discord-component-vote,       key "vote"
    - modal:        "feedback"        -> discord-modal-feedback,       key "feedback"
    - autocomplete: "weather"/"city"  -> discord-autocomplete-weather-city,
                                         key "weather city"
    """
    _, _, prefix = KIND_SPEC[kind]
    if kind == "command":
        name = prefix + "-".join(path_tokens)
        route_key = " ".join(path_tokens)
    elif kind == "autocomplete":
        name = prefix + "-".join([*path_tokens, option])
        route_key = " ".join([*path_tokens, option])
    else:  # component / modal: single-token prefix
        token = path_tokens[0]
        name = prefix + token
        route_key = token
    return name, route_key


# ---------------------------------------------------------------------------
# Schema stub (command kind)
# ---------------------------------------------------------------------------


def _find_or_create_option(options, name, otype):
    for opt in options:
        if isinstance(opt, dict) and opt.get("name") == name and opt.get("type") == otype:
            return opt
    node = {"name": name, "description": f"TODO: describe {name}", "type": otype}
    options.append(node)
    return node


def ensure_schema_path(commands, path_tokens):
    """Insert a schema stub for ``path_tokens`` into ``commands`` (mutates it).

    Nesting matches the application-command handler's path derivation:
    1 token -> top CHAT_INPUT command; 2 tokens -> command + subcommand (type 1);
    3 tokens -> command + group (type 2) + subcommand (type 1). Existing nodes
    are reused so re-running for a sibling path does not duplicate parents.
    """
    top = None
    for entry in commands:
        # Only reuse CHAT_INPUT entries (type absent or 1): Discord allows a
        # context-menu command (type 2/3) with the same name, and mutating one
        # of those here would corrupt it with description/options fields.
        if (
            isinstance(entry, dict)
            and entry.get("name") == path_tokens[0]
            and entry.get("type") in (None, 1)
        ):
            top = entry
            break
    if top is None:
        top = {"name": path_tokens[0], "description": f"TODO: describe {path_tokens[0]}"}
        commands.append(top)
    top.setdefault("description", f"TODO: describe {path_tokens[0]}")

    if len(path_tokens) == 1:
        return
    top.setdefault("options", [])
    if len(path_tokens) == 2:
        _find_or_create_option(top["options"], path_tokens[1], 1)
    else:  # 3 tokens
        group = _find_or_create_option(top["options"], path_tokens[1], 2)
        group.setdefault("options", [])
        _find_or_create_option(group["options"], path_tokens[2], 1)


# ---------------------------------------------------------------------------
# main.cpp templates (token substitution avoids brace-escaping headaches)
# ---------------------------------------------------------------------------


_WORKER_TEMPLATE = r'''#include <aws/lambda-runtime/runtime.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <exception>
#include <string>

#include "discord_interactions/errors.hpp"
#include "discord_interactions/interaction.hpp"
#include "discord_interactions/response.hpp"

// __FN__ — async worker for the "__ROUTE__" __KINDWORD__.
// The gateway has already sent a deferred ACK; this Lambda's return value is
// NOT user-visible. Deliver output by PATCHing @original. Return {"ok":true}
// only after the PATCH succeeds.

namespace {

using discord_interactions::json;

// Returns true when the user received the friendly copy (the interaction is
// handled), false when the message never reached Discord (missing ids, or the
// PATCH itself failed).
bool patch_friendly_error(const std::string& application_id, const std::string& token) {
    // Safe-error path: friendly, retryable copy. Never include internal detail
    // (exception messages, upstream bodies, SDK/JSON errors) in user-facing text.
    if (application_id.empty() || token.empty()) {
        return false;
    }
    try {
        discord_interactions::patch_original_response(
            application_id, token,
            // NOTE: visibility of this @original edit follows the deferred
            // ACK the gateway already sent — Discord ignores flags on webhook
            // edits. To make this command's replies ephemeral, set
            // "ephemeral_defer": true on its manifest route (AGENTS.md).
            json{{"content",
                  "Something went wrong handling that. Please try again."}});
        return true;
    } catch (const std::exception& patch_error) {
        std::fprintf(stderr, "__FN__ failed to PATCH error response: %s\n", patch_error.what());
        return false;
    }
}

aws::lambda_runtime::invocation_response handler(
    const aws::lambda_runtime::invocation_request& request) {
    json interaction = json::object();
    std::string application_id{};
    std::string token{};
    try {
        interaction = json::parse(request.payload);
        const discord_interactions::InteractionMetadata meta =
            discord_interactions::metadata(interaction);
        application_id = meta.application_id;
        token = meta.token;

        // TODO(__MODULE__): implement the "__ROUTE__" __KINDWORD__ here.
        // Validate user-controlled options at the boundary and raise a mapped
        // error for expected failures, e.g.:
        //   throw MODULE_ERROR("bad_input",
        //                      discord_interactions::ErrorCategory::validation,
        //                      "internal reason for logs only");
        // Visibility follows the deferred ACK (Discord ignores flags on
        // webhook edits); opt the route into "ephemeral_defer": true in the
        // module manifest if this command's replies must be ephemeral.
        json message = json{{"content", "TODO: reply from __FN__."}};

        // Success path: PATCH @original, then acknowledge the runtime.
        discord_interactions::patch_original_response(application_id, token, message);
        return aws::lambda_runtime::invocation_response::success(
            R"({"ok":true})", "application/json");
    } catch (const discord_interactions::ModuleError& error) {
        // Branch on error.category before treating a caught error as
        // deterministic. Only the deterministic categories (validation, auth)
        // are permanent, user-visible failures. The transient categories
        // (upstream, storage, configuration, rate_limited, internal) include
        // blips like patch_original_response throwing ErrorCategory::upstream on
        // a 429/5xx — those MUST fail so AWS async retry re-runs instead of
        // burning the interaction on a permanent "Something went wrong".
        std::fprintf(stderr, "__FN__ ModuleError code=%s category=%s: %s\n", error.code.c_str(), discord_interactions::error_category_name(error.category), error.what());
        const bool deterministic =
            error.category == discord_interactions::ErrorCategory::validation ||
            error.category == discord_interactions::ErrorCategory::auth;
        if (deterministic) {
            if (patch_friendly_error(application_id, token)) {
                // The user has their mapped, friendly response — the interaction
                // is handled. Return success so AWS async retry does not re-run
                // an already-user-visible, deterministic failure (AD-9 spirit).
                return aws::lambda_runtime::invocation_response::success(
                    R"({"ok":true})", "application/json");
            }
            // The friendly PATCH never reached Discord: fall through to failure
            // so the retry can try again to deliver a response.
        }
        // Transient category (or a deterministic case whose PATCH never landed):
        // fail so AWS async retry re-runs.
        return aws::lambda_runtime::invocation_response::failure(
            "module error", "ModuleError");
    } catch (const std::exception& ex) {
        // Unexpected failure: log to stderr, send generic retry copy.
        std::fprintf(stderr, "__FN__ unhandled exception: %s\n", ex.what());
        patch_friendly_error(application_id, token);
        return aws::lambda_runtime::invocation_response::failure(
            "unhandled exception", "std::exception");
    }
}

}  // namespace

int main() {
    aws::lambda_runtime::run_handler(handler);
    return 0;
}
'''


_AUTOCOMPLETE_TEMPLATE = r'''#include <aws/lambda-runtime/runtime.h>
#include <nlohmann/json.hpp>

#include <cstdio>
#include <exception>
#include <optional>
#include <string>

#include "discord_interactions/autocomplete.hpp"
#include "discord_interactions/errors.hpp"
#include "discord_interactions/interaction.hpp"

// __FN__ — sync autocomplete worker for option "__OPTION__" on "__PATH__".
// This sits on the synchronous interaction path: it must NOT sleep and must NOT
// PATCH. It returns the {type:8, data:{choices:[...]}} payload directly, which
// the autocomplete handler forwards verbatim to Discord.

namespace {

using discord_interactions::json;

aws::lambda_runtime::invocation_response handler(
    const aws::lambda_runtime::invocation_request& request) {
    try {
        const json interaction = json::parse(request.payload);
        const std::optional<std::string> query =
            discord_interactions::focused_option_value(interaction);

        // TODO(__MODULE__): build real suggestions for option "__OPTION__".
        json choices = json::array();
        choices.push_back(discord_interactions::choice("example", "example"));
        if (query.has_value()) {
            choices = discord_interactions::filter_choices(choices, *query);
        }

        const json response = discord_interactions::autocomplete_response(choices);
        return aws::lambda_runtime::invocation_response::success(
            response.dump(), "application/json");
    } catch (const discord_interactions::ModuleError& error) {
        // Log internals to stderr; fail safe with an empty choice list rather
        // than leaking internal detail to the user.
        std::fprintf(stderr, "__FN__ ModuleError code=%s: %s\n", error.code.c_str(), error.what());
        return aws::lambda_runtime::invocation_response::success(
            discord_interactions::autocomplete_response(json::array()).dump(),
            "application/json");
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "__FN__ unhandled exception: %s\n", ex.what());
        return aws::lambda_runtime::invocation_response::success(
            discord_interactions::autocomplete_response(json::array()).dump(),
            "application/json");
    }
}

}  // namespace

int main() {
    aws::lambda_runtime::run_handler(handler);
    return 0;
}
'''


_KIND_WORD = {
    "command": "command",
    "component": "message component",
    "modal": "modal submit",
}


def render_main_cpp(kind, name, module, route_key, path_tokens, option):
    if kind == "autocomplete":
        return (
            _AUTOCOMPLETE_TEMPLATE
            .replace("__FN__", name)
            .replace("__MODULE__", module)
            .replace("__OPTION__", option)
            .replace("__PATH__", " ".join(path_tokens))
        )
    return (
        _WORKER_TEMPLATE
        .replace("__FN__", name)
        .replace("__MODULE__", module)
        .replace("__ROUTE__", route_key)
        .replace("__KINDWORD__", _KIND_WORD[kind])
    )


# ---------------------------------------------------------------------------
# Orchestration
# ---------------------------------------------------------------------------


def build_plan(module_dir, kind, path_tokens, option):
    """Compute every action; validate inputs. Raises ModuleError on bad input."""
    route_kind, subdir, _ = KIND_SPEC[kind]
    name, route_key = derive(kind, path_tokens, option)
    lambda_rel = f"lambdas/{subdir}/{name}"

    manifest_path = module_dir / "module.manifest.json"
    if not manifest_path.exists():
        raise ModuleError(f"module manifest not found: {manifest_path}")
    manifest = read_json(manifest_path)
    if not isinstance(manifest, dict):
        raise ModuleError(f"{manifest_path} must contain a JSON object")

    routes = manifest.setdefault("routes", {})
    if not isinstance(routes, dict):
        raise ModuleError(f"{manifest_path} routes must be an object")
    existing = routes.get(route_kind, {})
    if isinstance(existing, dict) and route_key in existing:
        raise ModuleError(
            f"route {route_key!r} already exists under routes.{route_kind}; refusing"
        )

    # Refuse to clobber a hand-written main.cpp even when no route entry exists
    # yet: a folder with source but no route would otherwise be silently
    # overwritten (apply_plan writes unconditionally).
    main_cpp = module_dir / lambda_rel / "main.cpp"
    if main_cpp.exists():
        raise ModuleError(
            f"{main_cpp} already exists; refusing to overwrite it"
        )

    return {
        "name": name,
        "route_kind": route_kind,
        "route_key": route_key,
        "lambda_rel": lambda_rel,
        "main_cpp": module_dir / lambda_rel / "main.cpp",
        "manifest_path": manifest_path,
        "manifest": manifest,
    }


def apply_plan(plan, module_dir, kind, path_tokens, option):
    manifest = plan["manifest"]

    # 1) main.cpp
    main_cpp = plan["main_cpp"]
    main_cpp.parent.mkdir(parents=True, exist_ok=True)
    main_cpp.write_text(
        render_main_cpp(kind, plan["name"], manifest.get("name", module_dir.name),
                        plan["route_key"], path_tokens, option),
        encoding="utf-8",
    )

    # 2) manifest route (sorted keys) + lambdas list
    routes = manifest.setdefault("routes", {})
    mapping = dict(routes.get(plan["route_kind"], {}))
    mapping[plan["route_key"]] = plan["name"]
    routes[plan["route_kind"]] = dict(sorted(mapping.items()))

    lambdas = manifest.setdefault("lambdas", [])
    if plan["lambda_rel"] not in lambdas:
        lambdas.append(plan["lambda_rel"])

    plan["manifest_path"].write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )

    # 3) schema stub (command kind only)
    if kind == "command":
        commands_file = manifest.get("commands")
        if not isinstance(commands_file, str) or not commands_file:
            commands_file = "discord.commands.json"
            manifest["commands"] = commands_file
            plan["manifest_path"].write_text(
                json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
            )
        commands_path = module_dir / commands_file
        commands = []
        if commands_path.exists():
            loaded = read_json(commands_path)
            if isinstance(loaded, list):
                commands = loaded
        ensure_schema_path(commands, path_tokens)
        commands_path.parent.mkdir(parents=True, exist_ok=True)
        commands_path.write_text(json.dumps(commands, indent=2) + "\n", encoding="utf-8")


def print_plan(plan, kind, dry_run):
    verb = "Would create" if dry_run else "Created"
    print(f"{verb} {kind} Lambda: {plan['name']}")
    print(f"  main.cpp: {plan['main_cpp']}")
    print(f"  route:    routes.{plan['route_kind']}[{plan['route_key']!r}] -> "
          f"{plan['name']}")
    print(f"  lambdas:  {plan['lambda_rel']}")
    if kind == "command":
        print("  schema:   appended stub to discord.commands.json")


def print_checklist(plan):
    print("\nNext steps:")
    print(f"  1. Implement the TODO in {plan['main_cpp']}")
    print(f"  2. Build it:    scripts/build-lambda.sh "
          f"{plan['main_cpp'].parent}")
    print("  3. Validate:    python3 scripts/register-discord-commands.py "
          "--validate-only")
    print("  4. Register:    python3 scripts/register-discord-commands.py "
          "(guild-scoped while iterating)")


def build_arg_parser():
    parser = argparse.ArgumentParser(
        description="Scaffold a module Lambda skeleton (command/component/modal/autocomplete)."
    )
    parser.add_argument("--module", required=True, help="Module directory name under the modules root.")
    parser.add_argument("--kind", required=True, choices=sorted(KIND_SPEC),
                        help="Interaction kind to scaffold.")
    parser.add_argument("--path", required=True,
                        help='Command path ("group sub") or component/modal prefix (single token).')
    parser.add_argument("--option", default=None,
                        help="Autocomplete option name (required for --kind autocomplete).")
    parser.add_argument("--dry-run", action="store_true",
                        help="Print the planned actions without writing anything.")
    parser.add_argument("--modules-root", default=None,
                        help="Directory holding module subdirectories "
                             "(default: <repo>/modules).")
    return parser


def resolve_modules_root(arg):
    if arg:
        return Path(arg)
    return Path(__file__).resolve().parent.parent / "modules"


def run(args):
    path_tokens = args.path.split()
    if not path_tokens:
        raise ModuleError("--path must contain at least one token")

    if args.kind == "autocomplete":
        if not args.option:
            raise ModuleError("--kind autocomplete requires --option")
    if args.kind in ("component", "modal") and len(path_tokens) != 1:
        raise ModuleError(f"--kind {args.kind} --path must be a single-token prefix")
    if args.kind in ("component", "modal") and ":" in path_tokens[0]:
        # The routers split a component/modal custom_id at the first ':' to find
        # the prefix, so a prefix containing ':' could never match at runtime.
        raise ModuleError(
            f"--kind {args.kind} --path must not contain ':' — the router splits "
            "the custom_id at the first ':', so a prefix with ':' can never match"
        )
    if args.kind == "command" and len(path_tokens) > 3:
        raise ModuleError("--path may have at most 3 space-separated parts "
                          "(group -> subcommand is the deepest legal chain)")
    if args.kind == "command":
        # Fail fast on uppercase ASCII: Discord command names must be lowercase,
        # and the derived Lambda name is case-sensitive. Catch it here instead of
        # at the later --validate-only step.
        for token in path_tokens:
            if any("A" <= ch <= "Z" for ch in token):
                raise ModuleError(
                    f"--path token {token!r} contains uppercase ASCII; command "
                    "paths must be lowercase"
                )

    modules_root = resolve_modules_root(args.modules_root)
    module_dir = modules_root / args.module
    if not module_dir.exists():
        raise ModuleError(f"module directory not found: {module_dir}")

    plan = build_plan(module_dir, args.kind, path_tokens, args.option)

    if args.dry_run:
        print_plan(plan, args.kind, dry_run=True)
        return 0

    apply_plan(plan, module_dir, args.kind, path_tokens, args.option)
    print_plan(plan, args.kind, dry_run=False)
    print_checklist(plan)
    return 0


def main(argv=None):
    args = build_arg_parser().parse_args(argv)
    try:
        return run(args)
    except ModuleError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
