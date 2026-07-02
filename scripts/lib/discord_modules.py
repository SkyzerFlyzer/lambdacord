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
    routes = {}
    for module in discover_modules(repo_root):
        mapping = module["manifest"].get("routes", {}).get(route_kind, {})
        if not isinstance(mapping, dict):
            raise ModuleError(f"{module['manifest_path']} routes.{route_kind} must be an object")
        for route, function_name in mapping.items():
            if route in routes:
                raise ModuleError(f"duplicate {route_kind} route: {route}")
            routes[route] = function_name
    return routes


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
    return unicodedata.category(ch)[0] in ("L", "N")


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
        if isinstance(value, str) and not (lo <= len(value) <= hi):
            problems.append(
                f"{subject} {field}[{locale!r}] must be {lo}-{hi} characters"
            )


def _check_choices(option, subject: str, problems):
    choices = option.get("choices")
    if choices is None:
        return
    if not isinstance(choices, list):
        problems.append(f"{subject} choices must be an array")
        return
    if len(choices) > 25:
        problems.append(f"{subject} has {len(choices)} choices (maximum 25)")
    for choice in choices:
        if not isinstance(choice, dict):
            problems.append(f"{subject} has a choice that is not an object")
            continue
        cname = choice.get("name")
        if isinstance(cname, str) and len(cname) > 100:
            problems.append(f"{subject} choice name {cname!r} exceeds 100 characters")
        cvalue = choice.get("value")
        if isinstance(cvalue, str) and len(cvalue) > 100:
            problems.append(f"{subject} choice value exceeds 100 characters")


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

    seen_names = set()
    seen_optional = False
    for option in options:
        if not isinstance(option, dict):
            problems.append(f"{subject} has an option that is not an object")
            continue

        oname = option.get("name")
        otype = option.get("type")
        opt_subject = f"{subject} option {oname!r}"

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
            required = bool(option.get("required", False))
            if required and seen_optional:
                problems.append(
                    f"{opt_subject} is required and must come before optional options"
                )
            if not required:
                seen_optional = True
            _check_choices(option, opt_subject, problems)


def _check_command(command, module_name: str, problems):
    name = command.get("name")
    ctype = command.get("type", _CMD_CHAT_INPUT)
    subject = f"{module_name}: command {name!r}"

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
        isinstance(integration_types, list) and all(v in (0, 1) for v in integration_types)
    ):
        problems.append(f"{subject} integration_types must be a subset of {{0, 1}}")

    contexts = command.get("contexts")
    if contexts is not None and not (
        isinstance(contexts, list) and all(v in (0, 1, 2) for v in contexts)
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
    for command in commands:
        if not isinstance(command, dict):
            problems.append(f"{module_name}: each command must be a JSON object")
            continue
        name = command.get("name")
        if not isinstance(name, str) or not name:
            problems.append(f"{module_name}: each command must have a non-empty name")
            continue
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
    object form (``{"lambda": "discord-cmd-foo", ...}``) that a future
    route-config task may introduce, without a hard dependency on it.
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
    """
    errors = []
    warnings = []

    for module in modules:
        manifest = module["manifest"]
        module_name = manifest.get("name", "module")
        prefix = f"{module_name}:"

        commands = _load_module_commands_optional(module)
        if commands is None:
            continue

        routes = manifest.get("routes", {})
        if not isinstance(routes, dict):
            routes = {}

        existing_folders = _existing_folder_basenames(module)
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
                target = command_routes[path]
                if target != derived:
                    errors.append(
                        f"{prefix} commands route {path!r} points to {target!r} "
                        f"but the derived Lambda name is {derived!r}"
                    )
                if target not in existing_folders:
                    errors.append(
                        f"{prefix} commands route {path!r} target Lambda folder "
                        f"{target!r} does not exist in the module"
                    )

            # Route -> schema (reverse).
            for path in sorted(command_routes):
                if path not in schema_paths:
                    errors.append(
                        f"{prefix} commands route {path!r} has no matching command in the schema"
                    )

        # Autocomplete options must have a matching autocomplete route.
        autocomplete_targets = set(
            filter(None, _route_targets(routes.get("autocomplete", {})).values())
        )
        for path, option_name in autocompletes:
            derived = f"discord-autocomplete-{path.replace(' ', '-')}-{option_name}"
            if derived not in autocomplete_targets:
                errors.append(
                    f"{prefix} autocomplete option {option_name!r} on command {path!r} "
                    f"has no route {derived!r}"
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
                derived = kind_prefix + cmd_name.lower().replace(" ", "-")
                if cmd_name not in kind_routes:
                    errors.append(
                        f"{prefix} {kind} schema command {cmd_name!r} has no route entry"
                    )
                    continue
                target = kind_routes[cmd_name]
                if target != derived:
                    errors.append(
                        f"{prefix} {kind} route {cmd_name!r} points to {target!r} "
                        f"but the derived Lambda name is {derived!r}"
                    )
                if target not in existing_folders:
                    errors.append(
                        f"{prefix} {kind} route {cmd_name!r} target Lambda folder "
                        f"{target!r} does not exist in the module"
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
                if not isinstance(function_name, str) or not function_name:
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
