import json
import sys
import unicodedata
from pathlib import Path


class ModuleError(RuntimeError):
    pass


# Documented Discord locale identifiers (Discord "Locales" reference). Localized
# name/description maps may only key on these.
DISCORD_LOCALES = frozenset(
    {
        "id", "da", "de", "en-GB", "en-US", "es-ES", "es-419", "fr", "hr", "it",
        "lt", "hu", "nl", "no", "pl", "pt-BR", "ro", "fi", "sv-SE", "vi", "tr",
        "cs", "el", "bg", "ru", "uk", "hi", "th", "zh-CN", "ja", "zh-TW", "ko",
    }
)

# Discord application command types.
_CMD_CHAT_INPUT = 1
_CMD_USER = 2
_CMD_MESSAGE = 3

# Discord application command option types that introduce another nesting level.
_OPT_SUBCOMMAND = 1
_OPT_SUBCOMMAND_GROUP = 2

# Every valid Discord application command option type (SUB_COMMAND=1 ..
# ATTACHMENT=11). An option with a missing or out-of-range type would pass
# --validate-only as a leaf option, then be rejected by Discord's bulk overwrite.
_OPTION_TYPES = frozenset(range(1, 12))

# Manifest route-kind vocabulary. `user_commands` / `message_commands` are the
# context menu kinds (T3.1); `route_map` works for every kind listed here and
# manifest validation type-checks each kind's mapping.
ROUTE_KINDS = (
    "commands",
    "components",
    "modals",
    "autocomplete",
    "user_commands",
    "message_commands",
)

# Route kinds whose router has NO env route-map override: the modal and
# autocomplete routers always derive their worker Lambda name mechanically
# (discord-modal-<prefix> / discord-autocomplete-<path>-<option>). A
# non-mechanical target on these kinds is dead config that can never be
# invoked, so it is a hard ERROR rather than the supported-override WARNING that
# the map-backed kinds (commands / *_commands / components) get.
MAPLESS_ROUTE_KINDS = ("modals", "autocomplete")


def read_json(path: Path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise ModuleError(f"module file not found: {path}") from exc
    except json.JSONDecodeError as exc:
        raise ModuleError(f"invalid JSON in {path}: {exc}") from exc


def discover_modules(repo_root: Path):
    modules_root = repo_root / "modules"
    if not modules_root.exists():
        return []

    modules = []
    for manifest_path in sorted(modules_root.glob("*/module.manifest.json")):
        manifest = read_json(manifest_path)
        module_dir = manifest_path.parent
        name = manifest.get("name")
        if not isinstance(name, str) or not name:
            raise ModuleError(f"{manifest_path} must include a non-empty string name")
        modules.append({"dir": module_dir, "manifest_path": manifest_path, "manifest": manifest})
    return modules


def module_commands(module):
    manifest = module["manifest"]
    commands_file = manifest.get("commands")
    if not isinstance(commands_file, str) or not commands_file:
        raise ModuleError(f"{module['manifest_path']} must include a commands file")
    commands = read_json(module["dir"] / commands_file)
    if not isinstance(commands, list):
        raise ModuleError(f"{module['dir'] / commands_file} must contain a JSON array")
    return commands


def all_module_commands(repo_root: Path):
    commands = []
    for module in discover_modules(repo_root):
        commands.extend(module_commands(module))
    return commands


def lambda_dirs(repo_root: Path):
    dirs = [
        repo_root / "src" / "lambdas" / "discord-interactions",
        repo_root / "src" / "lambdas" / "discord-application-command-handler",
        repo_root / "src" / "lambdas" / "discord-message-component-handler",
        repo_root / "src" / "lambdas" / "discord-modal-handler",
        repo_root / "src" / "lambdas" / "discord-autocomplete-handler",
    ]

    for module in discover_modules(repo_root):
        manifest = module["manifest"]
        for rel in manifest.get("lambdas", []):
            if not isinstance(rel, str) or not rel:
                raise ModuleError(f"{module['manifest_path']} contains an invalid lambda path")
            dirs.append(module["dir"] / rel)
    return dirs


def route_map(repo_root: Path, route_kind: str):
    """Merge every installed module's ``routes.<route_kind>`` mapping.

    Route values may use either the plain string form (``"discord-cmd-x"``) or
    the object form (``{"lambda": "discord-cmd-x", "ephemeral_defer": true}``,
    T3.2). Object-form values are normalized to their target Lambda name so
    every consumer keeps seeing ``route -> function-name string``; values that
    fit neither form pass through unchanged (shape validation lives in
    :func:`validate_module_manifests`).
    """
    routes = {}
    for module in discover_modules(repo_root):
        mapping = module["manifest"].get("routes", {}).get(route_kind, {})
        if not isinstance(mapping, dict):
            raise ModuleError(f"{module['manifest_path']} routes.{route_kind} must be an object")
        for route, function_name in mapping.items():
            if route in routes:
                raise ModuleError(f"duplicate {route_kind} route: {route}")
            target = _route_target_name(function_name)
            routes[route] = target if target is not None else function_name
    return routes


# Route kinds whose object form may carry an ``ephemeral_defer`` flag. Slash
# commands opt in by their full command path; context menu commands
# (user_commands / message_commands) opt in by their raw command name, which is
# exactly the key the ingress matches a type-2/3 interaction against.
EPHEMERAL_DEFER_KINDS = ("commands", "user_commands", "message_commands")


def ephemeral_defer_routes(manifests):
    """Merged ephemeral-defer opt-ins across module manifests (T3.2 / AD-5).

    ``manifests`` is an iterable of parsed manifest dicts. Returns the sorted,
    deduplicated list of route keys whose object-form entry sets
    ``"ephemeral_defer": true`` — command paths from ``routes.commands`` and raw
    context-menu command names from ``routes.user_commands`` /
    ``routes.message_commands`` (the ingress matches context menus on the raw
    name). Plain-string routes and object routes without the flag (or with
    ``false``) contribute nothing; other route kinds never opt in.
    """
    paths = set()
    for manifest in manifests:
        if not isinstance(manifest, dict):
            continue
        routes = manifest.get("routes", {})
        if not isinstance(routes, dict):
            continue
        for kind in EPHEMERAL_DEFER_KINDS:
            mapping = routes.get(kind, {})
            if not isinstance(mapping, dict):
                continue
            for route, value in mapping.items():
                if not isinstance(route, str) or not route:
                    continue
                if isinstance(value, dict) and value.get("ephemeral_defer") is True:
                    paths.add(route)
    return sorted(paths)


# ---------------------------------------------------------------------------
# Discord-rule command schema validation (T4.1)
#
# These checks mirror what Discord's command-registration endpoint will reject,
# so authors catch problems at `register-discord-commands.py --validate-only`
# time instead of on a live bulk-overwrite. Errors are returned as plain strings
# and surfaced through the same aggregation mechanism as manifest validation
# (`validate_module_manifests` joins them with "; " into a ModuleError).
# ---------------------------------------------------------------------------


def _is_name_char(ch: str) -> bool:
    # Approximation of Discord's ^[-_\p{L}\p{N}]{1,32}$ name pattern without any
    # regex \p{} support (no third-party `regex` dependency): accept '-' and '_'
    # plus any character whose Unicode general category starts with 'L' (letter,
    # covering \p{L}) or 'N' (number, covering \p{N}). This is Unicode-aware and
    # matches Discord's intent for the common cases; it does not reproduce every
    # edge of Unicode property matching exactly.
    if ch in ("-", "_"):
        return True
    if unicodedata.category(ch)[0] in ("L", "N"):
        return True
    # Discord's real pattern also includes \p{sc=Deva}\p{sc=Thai}, which pull in
    # combining marks (general category M) that the L*/N* rule above misses.
    # Python's stdlib has no Unicode script properties, so approximate those two
    # scripts by their primary Unicode blocks — Devanagari U+0900–U+097F and Thai
    # U+0E00–U+0E7F — accepting any codepoint in range regardless of category.
    # This is a block approximation, not an exact \p{sc=...} match.
    cp = ord(ch)
    if 0x0900 <= cp <= 0x097F or 0x0E00 <= cp <= 0x0E7F:
        return True
    return False


def _chat_name_ok(name) -> bool:
    if not isinstance(name, str) or not (1 <= len(name) <= 32):
        return False
    return all(_is_name_char(ch) for ch in name)


def _length_ok(value, lo: int, hi: int) -> bool:
    return isinstance(value, str) and lo <= len(value) <= hi


def _check_localizations(loc, field: str, lo: int, hi: int, subject: str, problems):
    if loc is None:
        return
    if not isinstance(loc, dict):
        problems.append(f"{subject} {field} must be an object")
        return
    for locale, value in loc.items():
        if locale not in DISCORD_LOCALES:
            problems.append(f"{subject} {field} has unknown locale {locale!r}")
        # A non-string value would otherwise skip the length check silently and
        # be rejected only by Discord's bulk overwrite.
        if not isinstance(value, str):
            problems.append(f"{subject} {field}[{locale!r}] must be a string")
        elif not (lo <= len(value) <= hi):
            problems.append(
                f"{subject} {field}[{locale!r}] must be {lo}-{hi} characters"
            )


# Option types that support a static choices list, mapped to the Python type its
# choice `value` must have (STRING=3 -> str, INTEGER=4 -> int, NUMBER=10 -> float).
_CHOICE_VALUE_TYPE_NAME = {3: "STRING", 4: "INTEGER", 10: "NUMBER"}


def _choice_value_type_ok(otype, value) -> bool:
    if otype == 3:
        return isinstance(value, str)
    if otype == 4:
        return isinstance(value, int) and not isinstance(value, bool)
    if otype == 10:
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    # Other option types do not carry choices; leave value typing unchecked.
    return True


def _check_choices(option, subject: str, problems):
    choices = option.get("choices")
    otype = option.get("type")

    # autocomplete and a static choices list are mutually exclusive (Discord
    # rejects declaring both on the same option).
    if choices is not None and option.get("autocomplete") is True:
        problems.append(
            f"{subject} sets both autocomplete and choices, which are mutually exclusive"
        )

    # FIX 2: autocomplete is only legal on STRING(3)/INTEGER(4)/NUMBER(10). It
    # must be rejected on any other type even when no choices are declared.
    if option.get("autocomplete") is True and otype not in _CHOICE_VALUE_TYPE_NAME:
        problems.append(
            f"{subject} sets autocomplete on option type {otype!r}; "
            "autocomplete is only valid on STRING, INTEGER, or NUMBER options"
        )

    # FIX 3: static choices are only legal on STRING(3)/INTEGER(4)/NUMBER(10).
    # A non-choice option type carrying choices must be rejected (previously the
    # type check was skipped entirely for such options).
    if choices is not None and otype not in _CHOICE_VALUE_TYPE_NAME:
        problems.append(
            f"{subject} declares choices on option type {otype!r}; "
            "choices are only valid on STRING, INTEGER, or NUMBER options"
        )

    if choices is None:
        return
    if not isinstance(choices, list):
        problems.append(f"{subject} choices must be an array")
        return
    if len(choices) == 0:
        problems.append(f"{subject} choices must not be empty when present")
        return
    if len(choices) > 25:
        problems.append(f"{subject} has {len(choices)} choices (maximum 25)")
    for choice in choices:
        if not isinstance(choice, dict):
            problems.append(f"{subject} has a choice that is not an object")
            continue
        if "name" not in choice or "value" not in choice:
            problems.append(f"{subject} each choice must include a name and value")
            continue
        # FIX 5: a choice name must be a string of 1..100 chars. Previously only
        # overlong strings were caught, so "" and non-strings slipped through.
        cname = choice.get("name")
        if not isinstance(cname, str):
            problems.append(f"{subject} choice name must be a string")
        elif len(cname) == 0:
            problems.append(f"{subject} choice name must not be empty")
        elif len(cname) > 100:
            problems.append(f"{subject} choice name {cname!r} exceeds 100 characters")
        cvalue = choice.get("value")
        if isinstance(cvalue, str) and len(cvalue) > 100:
            problems.append(f"{subject} choice value exceeds 100 characters")
        # The choice value type must match the option type (Discord enforces
        # string values on STRING options, integers on INTEGER, numbers on NUMBER).
        if otype in _CHOICE_VALUE_TYPE_NAME and not _choice_value_type_ok(otype, cvalue):
            problems.append(
                f"{subject} choice value type must match the "
                f"{_CHOICE_VALUE_TYPE_NAME[otype]} option"
            )


def _check_options(options, container_type, subject: str, problems):
    # container_type is None at the top command level, or the option type of the
    # enclosing subcommand (1) / group (2). It drives the nesting-depth rule:
    # group -> subcommand is the deepest legal chain.
    if options is None:
        return
    if not isinstance(options, list):
        problems.append(f"{subject} options must be an array")
        return
    if len(options) > 25:
        problems.append(f"{subject} has {len(options)} options (maximum 25)")

    # Subcommands/groups (types 1/2) and leaf options must not coexist at the
    # same level — Discord rejects a command/group that mixes them.
    level_types = [
        opt.get("type") for opt in options if isinstance(opt, dict)
    ]
    has_container = any(t in (_OPT_SUBCOMMAND, _OPT_SUBCOMMAND_GROUP) for t in level_types)
    has_leaf = any(
        isinstance(t, int)
        and not isinstance(t, bool)
        and t in _OPTION_TYPES
        and t not in (_OPT_SUBCOMMAND, _OPT_SUBCOMMAND_GROUP)
        for t in level_types
    )
    if has_container and has_leaf:
        problems.append(
            f"{subject} must not mix subcommands/groups with other options "
            "at the same level"
        )

    seen_names = set()
    seen_optional = False
    for option in options:
        if not isinstance(option, dict):
            problems.append(f"{subject} has an option that is not an object")
            continue

        oname = option.get("name")
        otype = option.get("type")
        opt_subject = f"{subject} option {oname!r}"

        # An option must declare a supported integer type; otherwise it silently
        # reads as a leaf and passes --validate-only only to fail Discord later.
        if otype is None:
            problems.append(f"{opt_subject} is missing a type")
        elif (
            not isinstance(otype, int)
            or isinstance(otype, bool)
            or otype not in _OPTION_TYPES
        ):
            problems.append(f"{opt_subject} has unsupported type {otype!r}")

        # FIX 4: a SUB_COMMAND_GROUP (type 2) may only contain SUB_COMMAND
        # (type 1) children. A leaf option directly inside a group was
        # previously accepted; reject it. (A group nested in a group is caught
        # by the nesting-depth rule below, so it is excluded here.)
        if (
            container_type == _OPT_SUBCOMMAND_GROUP
            and isinstance(otype, int)
            and not isinstance(otype, bool)
            and otype in _OPTION_TYPES
            and otype not in (_OPT_SUBCOMMAND, _OPT_SUBCOMMAND_GROUP)
        ):
            problems.append(
                f"{opt_subject} is inside a subcommand group, which "
                "may only contain subcommands"
            )

        if not _chat_name_ok(oname):
            problems.append(
                f"{subject} option name {oname!r} must be 1-32 characters "
                "using only letters, digits, '-' or '_'"
            )
        elif oname.lower() != oname:
            problems.append(f"{subject} option name {oname!r} must be lowercase")

        if isinstance(oname, str):
            if oname in seen_names:
                problems.append(f"{subject} has a duplicate option name {oname!r}")
            seen_names.add(oname)

        if not _length_ok(option.get("description"), 1, 100):
            problems.append(f"{opt_subject} description must be 1-100 characters")

        _check_localizations(
            option.get("name_localizations"), "name_localizations", 1, 32, opt_subject, problems
        )
        _check_localizations(
            option.get("description_localizations"),
            "description_localizations",
            1,
            100,
            opt_subject,
            problems,
        )

        if otype in (_OPT_SUBCOMMAND, _OPT_SUBCOMMAND_GROUP):
            too_deep = container_type == _OPT_SUBCOMMAND or (
                container_type == _OPT_SUBCOMMAND_GROUP and otype == _OPT_SUBCOMMAND_GROUP
            )
            if too_deep:
                problems.append(
                    f"{opt_subject} nesting too deep "
                    "(group -> subcommand is the maximum)"
                )
            _check_options(option.get("options"), otype, opt_subject, problems)
        else:
            # FIX 6: "required" must be a boolean when present. A non-boolean
            # (e.g. the string "false") would otherwise coerce truthy via bool()
            # and silently distort the required-before-optional ordering check.
            raw_required = option.get("required", False)
            if "required" in option and not isinstance(raw_required, bool):
                problems.append(
                    f"{opt_subject} required must be a boolean when present"
                )
            required = bool(raw_required)
            if required and seen_optional:
                problems.append(
                    f"{opt_subject} is required and must come before optional options"
                )
            if not required:
                seen_optional = True
            _check_choices(option, opt_subject, problems)


_COMMAND_TYPES = frozenset((_CMD_CHAT_INPUT, _CMD_USER, _CMD_MESSAGE))


def _check_command(command, module_name: str, problems):
    name = command.get("name")
    subject = f"{module_name}: command {name!r}"

    # Top-level command type: absent means CHAT_INPUT (Discord defaults to 1);
    # when present it must be an integer in {1, 2, 3}. bool is an int subclass in
    # Python, so it is rejected as a non-integer (mirrors the option-type check).
    raw_type = command.get("type")
    if raw_type is None:
        ctype = _CMD_CHAT_INPUT
    elif not isinstance(raw_type, int) or isinstance(raw_type, bool):
        problems.append(f"{subject} command type must be an integer")
        return
    elif raw_type not in _COMMAND_TYPES:
        problems.append(f"{subject} has unsupported command type {raw_type!r}")
        return
    else:
        ctype = raw_type

    if ctype in (_CMD_USER, _CMD_MESSAGE):
        # Context menu commands: 1-32 char name (mixed case + spaces allowed),
        # and Discord forbids both descriptions and options on them.
        if not isinstance(name, str) or not (1 <= len(name) <= 32):
            problems.append(f"{subject} name must be 1-32 characters")
        if command.get("description") not in (None, ""):
            problems.append(f"{subject} must not have a description")
        if command.get("options"):
            problems.append(f"{subject} must not have options")
        _check_localizations(
            command.get("name_localizations"), "name_localizations", 1, 32, subject, problems
        )
        return

    # CHAT_INPUT (type absent or 1).
    if not _chat_name_ok(name):
        problems.append(
            f"{subject} name must be 1-32 characters "
            "using only letters, digits, '-' or '_'"
        )
    elif name.lower() != name:
        problems.append(f"{subject} name must be lowercase")

    if not _length_ok(command.get("description"), 1, 100):
        problems.append(f"{subject} description must be 1-100 characters")

    perms = command.get("default_member_permissions")
    if perms is not None and not (isinstance(perms, str) and perms.isdigit()):
        problems.append(
            f"{subject} default_member_permissions must be a string of digits"
        )

    integration_types = command.get("integration_types")
    if integration_types is not None and not (
        isinstance(integration_types, list)
        and all(isinstance(v, int) and not isinstance(v, bool) and v in (0, 1) for v in integration_types)
    ):
        problems.append(f"{subject} integration_types must be a subset of {{0, 1}}")

    contexts = command.get("contexts")
    if contexts is not None and not (
        isinstance(contexts, list)
        and all(isinstance(v, int) and not isinstance(v, bool) and v in (0, 1, 2) for v in contexts)
    ):
        problems.append(f"{subject} contexts must be a subset of {{0, 1, 2}}")

    _check_localizations(
        command.get("name_localizations"), "name_localizations", 1, 32, subject, problems
    )
    _check_localizations(
        command.get("description_localizations"),
        "description_localizations",
        1,
        100,
        subject,
        problems,
    )

    _check_options(command.get("options"), None, subject, problems)


def validate_command_schema(commands, module_name: str = "module"):
    """Return a list of Discord-rule violations found in a command schema array.

    Empty list means the schema is acceptable to Discord for the rules covered
    here. Callers aggregate these strings alongside manifest problems.
    """
    problems = []
    if not isinstance(commands, list):
        return problems
    seen_command_keys = set()
    for command in commands:
        if not isinstance(command, dict):
            problems.append(f"{module_name}: each command must be a JSON object")
            continue
        name = command.get("name")
        if not isinstance(name, str) or not name:
            problems.append(f"{module_name}: each command must have a non-empty name")
            continue
        # Discord treats (name, type) as the identity of a top-level command;
        # duplicates of the same name and type collide on bulk overwrite. The
        # default type is CHAT_INPUT (1). Use repr(type) so a non-hashable/odd
        # type value cannot raise here.
        command_key = (name, repr(command.get("type", _CMD_CHAT_INPUT)))
        if command_key in seen_command_keys:
            problems.append(f"{module_name}: duplicate command name {name!r}")
        seen_command_keys.add(command_key)
        _check_command(command, module_name, problems)
    return problems


def _load_module_commands_optional(module):
    """Read a module's declared command schema if the file is present and valid.

    Returns the parsed list, or ``None`` when the manifest declares no commands
    file, the file is absent on disk, or its contents are not a JSON array.
    Never raises: schema *content* validation is advisory here (manifest-level
    validation stays characterization-stable).
    """
    manifest = module["manifest"]
    commands_file = manifest.get("commands")
    if not isinstance(commands_file, str) or not commands_file:
        return None
    path = module["dir"] / commands_file
    if not path.exists():
        return None
    try:
        commands = read_json(path)
    except ModuleError:
        return None
    return commands if isinstance(commands, list) else None


def command_count_by_module(repo_root: Path):
    """Report the total command count per module (informational, not an error).

    Modules whose commands file is absent report ``0``.
    """
    counts = {}
    for module in discover_modules(repo_root):
        commands = _load_module_commands_optional(module)
        counts[module["manifest"]["name"]] = len(commands) if commands else 0
    return counts


# ---------------------------------------------------------------------------
# Route <-> schema <-> Lambda-folder consistency (T4.2)
#
# The "naming convention is load-bearing": the application-command handler
# derives the target Lambda name mechanically from a command path
# (`<group> <subcommand>` -> `discord-cmd-<group>-<subcommand>`). This check
# catches drift between a module's command schema, its manifest route map, and
# the Lambda folders on disk before a live registration/deploy. Errors join the
# same aggregation as `validate_module_manifests`; an unreferenced Lambda folder
# is surfaced as a WARNING (returned separately, never a validation failure).
# ---------------------------------------------------------------------------


def _route_target_name(value):
    """Return the target Lambda name for a manifest route value.

    Tolerates both the plain-string form (``"discord-cmd-foo"``) and the
    object form (``{"lambda": "discord-cmd-foo", "ephemeral_defer": true}``,
    T3.2). Returns ``None`` for values that fit neither form.
    """
    if isinstance(value, str):
        return value
    if isinstance(value, dict):
        lam = value.get("lambda")
        if isinstance(lam, str):
            return lam
    return None


def _collect_schema_paths_and_autocompletes(commands):
    """Derive chat-input command paths and autocomplete option targets.

    Mirrors the application-command handler's derivation exactly: a CHAT_INPUT
    command contributes a path only when it is nested (a subcommand) or has no
    subcommand/group options; groups (type 2) and subcommands (type 1) are
    walked to their leaves. Returns ``(paths, autocompletes)`` where ``paths``
    is a set of space-joined command paths and ``autocompletes`` is a list of
    ``(path, option_name)`` pairs for options flagged ``"autocomplete": true``.
    """
    paths = set()
    autocompletes = []

    def walk(command_list, prefix):
        for command in command_list:
            if not isinstance(command, dict):
                continue
            name = command.get("name")
            if not isinstance(name, str) or not name:
                continue
            ctype = command.get("type", _CMD_CHAT_INPUT)
            current = [*prefix, name]
            options = command.get("options", [])
            if not isinstance(options, list):
                options = []
            sub_options = [
                option
                for option in options
                if isinstance(option, dict)
                and option.get("type") in (_OPT_SUBCOMMAND, _OPT_SUBCOMMAND_GROUP)
            ]

            if ctype == _CMD_CHAT_INPUT and (prefix or not sub_options):
                path = " ".join(current)
                paths.add(path)
                for option in options:
                    if not isinstance(option, dict):
                        continue
                    if option.get("type") in (_OPT_SUBCOMMAND, _OPT_SUBCOMMAND_GROUP):
                        continue
                    if option.get("autocomplete") is True:
                        oname = option.get("name")
                        if isinstance(oname, str) and oname:
                            autocompletes.append((path, oname))

            for option in options:
                if isinstance(option, dict) and option.get("type") in (
                    _OPT_SUBCOMMAND,
                    _OPT_SUBCOMMAND_GROUP,
                ):
                    walk([option], current)

    walk(commands, [])
    return paths, autocompletes


def _existing_folder_basenames(module):
    """Basenames of the module's declared Lambda folders that exist on disk."""
    module_dir = module["dir"]
    existing = set()
    for rel in module["manifest"].get("lambdas", []):
        if isinstance(rel, str) and rel and (module_dir / rel / "main.cpp").exists():
            existing.add(Path(rel).name)
    return existing


def _route_targets(mapping):
    """Return the dict of ``route -> target-name`` for a manifest route map."""
    targets = {}
    if isinstance(mapping, dict):
        for route, value in mapping.items():
            if isinstance(route, str) and route:
                targets[route] = _route_target_name(value)
    return targets


# ASCII-only lowercase table (A-Z -> a-z). See ``context_menu_route_suffix`` in
# src/include/discord_interactions/interaction.hpp: the C++ router lowercases
# ASCII bytes only and leaves multibyte UTF-8 untouched. Python's ``str.lower()``
# case-folds non-ASCII too ("Über" -> "über"), which would derive a different
# Lambda name than the router actually invokes — so every context-menu
# name->lambda derivation must go through this ASCII-only path to stay
# byte-identical to the router.
_ASCII_LOWER = str.maketrans(
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ", "abcdefghijklmnopqrstuvwxyz"
)


def context_menu_route_suffix(name: str) -> str:
    """Mirror the C++ ``context_menu_route_suffix``: ASCII-only lowercase +
    spaces -> '-', leaving multibyte UTF-8 bytes untouched."""
    return name.translate(_ASCII_LOWER).replace(" ", "-")


def check_route_consistency(modules):
    """Cross-check command schemas, manifest routes, and Lambda folders.

    ``modules`` is the list returned by :func:`discover_modules`. Returns
    ``{"errors": [...], "warnings": [...]}`` with every message prefixed by the
    owning module name. Errors are hard consistency failures; warnings flag an
    unreferenced Lambda folder without failing validation.

    Schema/command-route consistency is enforced for a module once it declares
    at least one command route (i.e. it has opted into routing); a module with a
    schema but no command routes is treated as not-yet-wired and only its
    autocomplete and orphan-folder checks run. Context-menu route kinds
    (``user_commands`` / ``message_commands``) are validated when present and
    tolerated when absent, so this check does not hard-depend on T3.1.

    Route-map overrides are a supported feature (T4.2 decision): for
    ``commands`` / ``user_commands`` / ``message_commands`` / ``autocomplete`` /
    ``components`` / ``modals``, a route target that EQUALS the mechanical
    derivation keeps the folder-existence ERROR, while a target that DIFFERS is
    a WARNING ("non-mechanical route target ... ensure it is deployed and
    IAM-granted") — the Terraform grants invoke on every manifest route target.
    """
    errors = []
    warnings = []

    def _override_warning(prefix, kind, key, target, derived):
        return (
            f"{prefix} {kind} route {key!r} has a non-mechanical route target "
            f"{target!r} (mechanical derivation is {derived!r}); ensure it is "
            "deployed and IAM-granted"
        )

    def _check_mechanical_target(prefix, kind, key, target, derived, existing_folders):
        # Mechanical target -> folder must exist (error). Non-mechanical target
        # -> supported override (warning). ``target is None`` means a malformed
        # route value already flagged by manifest validation; skip it here.
        if target is None:
            return
        if target == derived:
            if target not in existing_folders:
                errors.append(
                    f"{prefix} {kind} route {key!r} target Lambda folder "
                    f"{target!r} does not exist in the module"
                )
        elif kind in MAPLESS_ROUTE_KINDS:
            # No env route-map override exists for these routers, so a
            # non-mechanical target is dead config (FIX 1): hard ERROR.
            errors.append(
                f"{prefix} {kind} route {key!r} has a non-mechanical route "
                f"target {target!r}, but the {kind} router has no route-map "
                f"override; the target must equal the mechanical derivation "
                f"{derived!r}"
            )
        else:
            warnings.append(_override_warning(prefix, kind, key, target, derived))

    for module in modules:
        manifest = module["manifest"]
        module_name = manifest.get("name", "module")
        prefix = f"{module_name}:"

        routes = manifest.get("routes", {})
        if not isinstance(routes, dict):
            routes = {}
        existing_folders = _existing_folder_basenames(module)

        # Component/modal routes have no schema to cross-check, but their target
        # names are still mechanically derived (discord-component-<prefix> /
        # discord-modal-<prefix>). Apply the same mechanical-vs-override policy.
        # These run regardless of whether a command schema is present.
        for kind, name_prefix in (
            ("components", "discord-component-"),
            ("modals", "discord-modal-"),
        ):
            for key, target in sorted(_route_targets(routes.get(kind, {})).items()):
                derived = name_prefix + key
                _check_mechanical_target(
                    prefix, kind, key, target, derived, existing_folders
                )

        commands = _load_module_commands_optional(module)
        if commands is None:
            continue

        schema_paths, autocompletes = _collect_schema_paths_and_autocompletes(commands)

        command_routes = _route_targets(routes.get("commands", {}))

        # Schema -> route -> folder. Only enforced once the module declares a
        # command route (see the docstring): a route-less schema is not-yet-wired.
        if command_routes:
            for path in sorted(schema_paths):
                derived = "discord-cmd-" + path.replace(" ", "-")
                if path not in command_routes:
                    errors.append(
                        f"{prefix} command schema path {path!r} has no commands route entry"
                    )
                    continue
                _check_mechanical_target(
                    prefix, "commands", path, command_routes[path], derived,
                    existing_folders,
                )

            # Route -> schema (reverse).
            for path in sorted(command_routes):
                if path not in schema_paths:
                    errors.append(
                        f"{prefix} commands route {path!r} has no matching command in the schema"
                    )

        # Autocomplete options must have a matching autocomplete route, keyed by
        # the "<path> <option>" route key (mirrors the command derivation).
        autocomplete_routes = _route_targets(routes.get("autocomplete", {}))
        for path, option_name in autocompletes:
            route_key = f"{path} {option_name}"
            derived = f"discord-autocomplete-{path.replace(' ', '-')}-{option_name}"
            if route_key not in autocomplete_routes:
                errors.append(
                    f"{prefix} autocomplete option {option_name!r} on command {path!r} "
                    f"has no route entry (expected route {route_key!r} -> {derived!r})"
                )
                continue
            _check_mechanical_target(
                prefix, "autocomplete", route_key, autocomplete_routes[route_key],
                derived, existing_folders,
            )

        # Context-menu route kinds (validate if present, tolerate absent).
        for kind, kind_prefix, cmd_type in (
            ("user_commands", "discord-usercmd-", _CMD_USER),
            ("message_commands", "discord-msgcmd-", _CMD_MESSAGE),
        ):
            kind_routes = _route_targets(routes.get(kind, {}))
            if not kind_routes:
                continue
            schema_names = {
                command.get("name")
                for command in commands
                if isinstance(command, dict)
                and command.get("type") == cmd_type
                and isinstance(command.get("name"), str)
            }
            for cmd_name in sorted(schema_names):
                derived = kind_prefix + context_menu_route_suffix(cmd_name)
                if cmd_name not in kind_routes:
                    errors.append(
                        f"{prefix} {kind} schema command {cmd_name!r} has no route entry"
                    )
                    continue
                _check_mechanical_target(
                    prefix, kind, cmd_name, kind_routes[cmd_name], derived,
                    existing_folders,
                )
            for route_name in sorted(kind_routes):
                if route_name not in schema_names:
                    errors.append(
                        f"{prefix} {kind} route {route_name!r} has no matching command in the schema"
                    )

        # Orphan Lambda folders -> warning. A folder is referenced if any route
        # kind targets it, or the error mapper points at it.
        referenced = set()
        for route_kind in ROUTE_KINDS:
            referenced.update(filter(None, _route_targets(routes.get(route_kind, {})).values()))
        error_mapper = manifest.get("error_mapper", {})
        if isinstance(error_mapper, dict):
            mapper_path = error_mapper.get("path")
            if isinstance(mapper_path, str) and mapper_path:
                referenced.add(Path(mapper_path).name)

        for rel in manifest.get("lambdas", []):
            if isinstance(rel, str) and rel and Path(rel).name not in referenced:
                warnings.append(
                    f"{prefix} Lambda folder {rel!r} is not referenced by any route"
                )

    return {"errors": errors, "warnings": warnings}


def validate_module_manifests(repo_root: Path):
    problems = []
    modules = discover_modules(repo_root)
    for module in modules:
        manifest = module["manifest"]
        module_dir = module["dir"]
        routes = manifest.get("routes", {})
        lambdas = set(manifest.get("lambdas", []))

        for rel in sorted(lambdas):
            lambda_dir = module_dir / rel
            if not (lambda_dir / "main.cpp").exists():
                problems.append(f"{manifest['name']} lambda missing main.cpp: {lambda_dir}")

        for route_kind in ROUTE_KINDS:
            mapping = routes.get(route_kind, {})
            if not isinstance(mapping, dict):
                problems.append(f"{module['manifest_path']} routes.{route_kind} must be an object")
                continue
            for route, function_name in mapping.items():
                if not isinstance(route, str) or not route:
                    problems.append(f"{module['manifest_path']} has an invalid {route_kind} route")
                if isinstance(function_name, str) and function_name:
                    continue
                if isinstance(function_name, dict):
                    # Object route form (T3.2): requires a non-empty string
                    # "lambda"; "ephemeral_defer" must be a boolean and is only
                    # legal on command routes.
                    target = function_name.get("lambda")
                    if not isinstance(target, str) or not target:
                        problems.append(
                            f"{module['manifest_path']} route {route!r} object form "
                            'must include a non-empty string "lambda"'
                        )
                    if "ephemeral_defer" in function_name:
                        if route_kind not in EPHEMERAL_DEFER_KINDS:
                            problems.append(
                                f"{module['manifest_path']} route {route!r} sets "
                                f"ephemeral_defer on routes.{route_kind} "
                                "(only command and context-menu routes may opt in)"
                            )
                        elif not isinstance(function_name["ephemeral_defer"], bool):
                            problems.append(
                                f"{module['manifest_path']} route {route!r} "
                                "ephemeral_defer must be a boolean"
                            )
                        elif function_name["ephemeral_defer"] is True and (
                            isinstance(route, str) and "," in route
                        ):
                            # The opted-in keys are joined into the
                            # DISCORD_EPHEMERAL_DEFER_ROUTES CSV allowlist; a comma
                            # in the key would split it into two bogus entries.
                            problems.append(
                                f"{module['manifest_path']} route {route!r} sets "
                                "ephemeral_defer but its key contains a comma, "
                                "which breaks the DISCORD_EPHEMERAL_DEFER_ROUTES "
                                "CSV allowlist"
                            )
                    continue
                problems.append(
                    f"{module['manifest_path']} route {route!r} has an invalid function name"
                )

        error_mapper = manifest.get("error_mapper", {})
        mapper_path = error_mapper.get("path")
        if isinstance(mapper_path, str) and mapper_path:
            if not (module_dir / mapper_path / "main.cpp").exists():
                problems.append(f"{manifest['name']} error mapper missing main.cpp: {mapper_path}")

        errors_file = manifest.get("errors")
        if isinstance(errors_file, str) and errors_file:
            errors = read_json(module_dir / errors_file)
            if not isinstance(errors.get("errors"), dict):
                problems.append(f"{module_dir / errors_file} must contain an errors object")

        terraform_dir = manifest.get("terraform")
        if isinstance(terraform_dir, str) and terraform_dir:
            if not (module_dir / terraform_dir / "main.tf").exists():
                problems.append(f"{manifest['name']} terraform missing main.tf: {terraform_dir}")

        # Discord-rule command schema validation (T4.1). Only runs when the
        # declared commands file is present and is a JSON array, so a manifest
        # that merely names a not-yet-written schema stays valid at this layer.
        commands = _load_module_commands_optional(module)
        if commands is not None:
            problems.extend(validate_command_schema(commands, manifest["name"]))

    # Route <-> schema <-> Lambda-folder consistency (T4.2). Errors join the
    # aggregation; warnings are surfaced to stderr without failing validation.
    consistency = check_route_consistency(modules)
    problems.extend(consistency["errors"])
    for warning in consistency["warnings"]:
        print(f"WARNING: {warning}", file=sys.stderr)

    if problems:
        raise ModuleError("; ".join(problems))
