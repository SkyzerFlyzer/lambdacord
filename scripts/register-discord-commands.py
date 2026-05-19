#!/usr/bin/env python3

import argparse
import json
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "lib"))
from discord_modules import (  # noqa: E402
    ModuleError,
    all_module_commands,
    discover_modules,
    validate_module_manifests,
)


DEFAULT_USER_AGENT = "DiscordBot (https://github.com/SkyzerFlyzer/lambdacord, 1.0)"
DEFAULT_DISCORD_API_VERSION = "10"


def discord_api_base_url() -> str:
    base_url = os.getenv("DISCORD_API_BASE_URL", "").rstrip("/")
    if base_url:
        return base_url

    version = os.getenv("DISCORD_API_VERSION", DEFAULT_DISCORD_API_VERSION).strip()
    if not version:
        version = DEFAULT_DISCORD_API_VERSION
    return f"https://discord.com/api/v{version}"


def load_dotenv(path: Path):
    if not path.exists():
        return

    for line_number, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue

        if line.startswith("export "):
            line = line.removeprefix("export ").strip()

        key, separator, value = line.partition("=")
        key = key.strip()
        if not separator or not key:
            raise RuntimeError(f"invalid .env entry at {path}:{line_number}")

        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
            value = value[1:-1]

        os.environ.setdefault(key, value)


def read_commands(path: Path):
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as exc:
        raise RuntimeError(f"commands file not found: {path}") from exc
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"invalid JSON in commands file {path}: {exc}") from exc

    if not isinstance(data, list):
        raise RuntimeError("commands file must contain a JSON array")
    return data


def collect_command_paths(commands, prefix=None):
    if prefix is None:
        prefix = []

    paths = set()
    for command in commands:
        if not isinstance(command, dict):
            raise RuntimeError("each command definition must be a JSON object")

        name = command.get("name")
        if not isinstance(name, str) or not name:
            raise RuntimeError("each command definition must include a non-empty string name")

        command_type = command.get("type", 1)
        current_path = [*prefix, name]
        options = command.get("options", [])

        subcommand_options = [
            option
            for option in options
            if isinstance(option, dict) and option.get("type") in (1, 2)
        ]

        if command_type == 1 and (prefix or not subcommand_options):
            paths.add(" ".join(current_path))

        if isinstance(options, list):
            for option in options:
                if not isinstance(option, dict):
                    continue
                option_type = option.get("type")
                if option_type in (1, 2):
                    paths.update(collect_command_paths([option], current_path))

    return paths


def collect_command_lambdas(commands_root: Path):
    if not commands_root.exists():
        return set()

    return {
        path.name
        for path in commands_root.iterdir()
        if path.is_dir() and path.name.startswith("discord-cmd-") and (path / "main.cpp").exists()
    }


def validate_command_mappings(commands, commands_root: Path):
    defined_paths = collect_command_paths(commands)
    registered_lambdas = {"discord-cmd-" + path.replace(" ", "-") for path in defined_paths}
    existing_lambdas = collect_command_lambdas(commands_root)

    missing_from_manifest = sorted(existing_lambdas - registered_lambdas)
    missing_lambda_dirs = sorted(registered_lambdas - existing_lambdas)

    if missing_from_manifest or missing_lambda_dirs:
        problems = []
        if missing_from_manifest:
            problems.append(
                "command Lambdas missing from the Discord command manifest: "
                + ", ".join(missing_from_manifest)
            )
        if missing_lambda_dirs:
            problems.append(
                "Discord command manifest entries without matching Lambda folders: "
                + ", ".join(missing_lambda_dirs)
            )
        raise RuntimeError("; ".join(problems))


def validate_custom_commands(commands):
    collect_command_paths(commands)


def validate_module_command_mappings(repo_root: Path):
    validate_module_manifests(repo_root)

    modules = discover_modules(repo_root)
    registered_routes = set()
    for module in modules:
        routes = module["manifest"].get("routes", {}).get("commands", {})
        registered_routes.update(routes.keys())

    defined_paths = collect_command_paths(all_module_commands(repo_root))
    missing_from_schema = sorted(registered_routes - defined_paths)
    missing_from_routes = sorted(defined_paths - registered_routes)
    if missing_from_schema or missing_from_routes:
        problems = []
        if missing_from_schema:
            problems.append(
                "module command routes missing from Discord command schemas: "
                + ", ".join(missing_from_schema)
            )
        if missing_from_routes:
            problems.append(
                "Discord command schemas missing module routes: "
                + ", ".join(missing_from_routes)
            )
        raise RuntimeError("; ".join(problems))


def strip_global_only_fields(commands):
    sanitized = []
    for command in commands:
        item = dict(command)
        item.pop("contexts", None)
        item.pop("integration_types", None)
        sanitized.append(item)
    return sanitized


def bulk_overwrite_commands(
    bot_token: str,
    app_id: str,
    commands,
    guild_id: str | None,
    user_agent: str,
):
    base_url = discord_api_base_url()
    if guild_id:
        url = f"{base_url}/applications/{app_id}/guilds/{guild_id}/commands"
        scope = f"guild ({guild_id})"
    else:
        url = f"{base_url}/applications/{app_id}/commands"
        scope = "global"

    payload = json.dumps(commands).encode("utf-8")
    request = urllib.request.Request(
        url=url,
        data=payload,
        method="PUT",
        headers={
            "Authorization": f"Bot {bot_token}",
            "Content-Type": "application/json",
            "User-Agent": user_agent,
        },
    )

    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            body = response.read().decode("utf-8")
            result = json.loads(body) if body else []
    except urllib.error.HTTPError as exc:
        error_body = exc.read().decode("utf-8", errors="replace")
        hint = ""
        if exc.code == 403 and "error code: 1010" in error_body:
            hint = (
                " Discord returned a Cloudflare 1010 block before the API handler. "
                "Check that the request is sending a Discord-style User-Agent."
            )
        raise RuntimeError(
            f"Discord API error (HTTP {exc.code}) while overwriting {scope} commands: "
            f"{error_body}{hint}"
        ) from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"Failed to reach Discord API: {exc}") from exc

    print(f"Updated {len(result)} {scope} command(s).")
    for item in result:
        name = item.get("name", "<unknown>")
        cmd_id = item.get("id", "<no-id>")
        print(f"- {name} (id: {cmd_id})")


def main():
    repo_root = Path(__file__).resolve().parent.parent
    load_dotenv(repo_root / ".env")

    parser = argparse.ArgumentParser(
        description="Bulk-overwrite Discord slash commands from a JSON file."
    )
    parser.add_argument(
        "--commands-file",
        default=None,
        help="Path to JSON array of command definitions. Defaults to merged module schemas.",
    )
    parser.add_argument(
        "--guild-id",
        default=os.getenv("DISCORD_GUILD_ID"),
        help="Guild ID for instant command updates. If omitted, registers globally.",
    )
    parser.add_argument(
        "--application-id",
        default=os.getenv("DISCORD_APPLICATION_ID"),
        help="Discord application ID (or set DISCORD_APPLICATION_ID).",
    )
    parser.add_argument(
        "--bot-token",
        default=os.getenv("DISCORD_BOT_TOKEN"),
        help="Discord bot token (or set DISCORD_BOT_TOKEN).",
    )
    parser.add_argument(
        "--user-agent",
        default=os.getenv("DISCORD_API_USER_AGENT", DEFAULT_USER_AGENT),
        help="User-Agent to send to the Discord API (or set DISCORD_API_USER_AGENT).",
    )
    parser.add_argument(
        "--skip-validation",
        action="store_true",
        help="Skip checking that command Lambdas and command definitions match.",
    )
    parser.add_argument(
        "--validate-only",
        action="store_true",
        help="Validate command definitions against command Lambda folders without calling Discord.",
    )
    args = parser.parse_args()

    if args.commands_file:
        commands = read_commands(Path(args.commands_file))
    else:
        commands = all_module_commands(repo_root)

    if not args.skip_validation:
        if args.commands_file:
            validate_custom_commands(commands)
        else:
            validate_module_command_mappings(repo_root)
        print("Command manifest validation passed.")

    if args.validate_only:
        return

    if not args.application_id:
        raise RuntimeError("Missing application id. Use --application-id or DISCORD_APPLICATION_ID.")
    if not args.bot_token:
        raise RuntimeError("Missing bot token. Use --bot-token or DISCORD_BOT_TOKEN.")
    if args.guild_id:
        commands = strip_global_only_fields(commands)
    bulk_overwrite_commands(
        args.bot_token,
        args.application_id,
        commands,
        args.guild_id,
        args.user_agent,
    )


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, ModuleError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
