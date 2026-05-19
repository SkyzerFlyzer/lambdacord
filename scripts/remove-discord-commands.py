#!/usr/bin/env python3

import argparse
import json
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path


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


def overwrite_commands(
    bot_token: str,
    app_id: str,
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

    request = urllib.request.Request(
        url=url,
        data=b"[]",
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
            f"Discord API error (HTTP {exc.code}) while removing {scope} commands: "
            f"{error_body}{hint}"
        ) from exc
    except urllib.error.URLError as exc:
        raise RuntimeError(f"Failed to reach Discord API: {exc}") from exc

    print(f"Removed all {scope} commands. Discord now reports {len(result)} remaining command(s).")


def main():
    repo_root = Path(__file__).resolve().parent.parent
    load_dotenv(repo_root / ".env")

    parser = argparse.ArgumentParser(
        description="Remove all registered Discord slash commands globally or for one guild."
    )
    parser.add_argument(
        "--guild-id",
        default=os.getenv("DISCORD_GUILD_ID"),
        help="Guild ID to clear guild-scoped commands. If omitted, clears global commands.",
    )
    parser.add_argument(
        "--global",
        action="store_true",
        dest="clear_global",
        help="Clear global commands even if DISCORD_GUILD_ID is set.",
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
    args = parser.parse_args()

    if not args.application_id:
        raise RuntimeError("Missing application id. Use --application-id or DISCORD_APPLICATION_ID.")
    if not args.bot_token:
        raise RuntimeError("Missing bot token. Use --bot-token or DISCORD_BOT_TOKEN.")

    guild_id = None if args.clear_global else args.guild_id
    overwrite_commands(args.bot_token, args.application_id, guild_id, args.user_agent)


if __name__ == "__main__":
    try:
        main()
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
