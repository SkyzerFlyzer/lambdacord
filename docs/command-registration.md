# Command Registration

`scripts/register-discord-commands.py` discovers every
`modules/*/module.manifest.json`, merges the installed module command
schemas, and sends a Discord bulk overwrite. Before registering, it validates
module manifests, route maps, command schemas, Lambda folders, error
schemas, and Terraform folders.

## Validate without calling Discord

```bash
python3 scripts/register-discord-commands.py --validate-only
```

## Register commands

Guild-scoped registration is instant — use it while iterating:

```bash
DISCORD_BOT_TOKEN="your-bot-token" \
DISCORD_APPLICATION_ID="your-app-id" \
DISCORD_GUILD_ID="your-guild-id" \
python3 scripts/register-discord-commands.py
```

Global registration (omit the guild id):

```bash
DISCORD_BOT_TOKEN="your-bot-token" \
DISCORD_APPLICATION_ID="your-app-id" \
python3 scripts/register-discord-commands.py
```

Pass `--commands-file <path>` only when intentionally registering a custom
schema, or `--skip-validation` if you intentionally want a partial manifest.

## Remove commands

Clear guild-scoped commands:

```bash
DISCORD_BOT_TOKEN="your-bot-token" \
DISCORD_APPLICATION_ID="your-app-id" \
DISCORD_GUILD_ID="your-guild-id" \
python3 scripts/remove-discord-commands.py
```

Clear global commands:

```bash
DISCORD_BOT_TOKEN="your-bot-token" \
DISCORD_APPLICATION_ID="your-app-id" \
python3 scripts/remove-discord-commands.py --global
```

## Inspect the merged route map

```bash
python3 scripts/print-module-route-map.py commands
python3 scripts/print-module-route-map.py components
```

## Convenience env file

Copy `.env.example` to `.env` if you want a local place to keep Discord
command registration values.

## Discord API version

Discord REST calls default to API version `10`, which is currently the
latest available version in Discord's official API reference. Set
`DISCORD_API_VERSION` to use a different version, or set
`DISCORD_API_BASE_URL` to override the full API base URL for local tests
(the base-URL override takes precedence).
