# Modules

Feature modules are locally installed repos under `modules/<name>/`. The
parent repo ignores `modules/`; module repos are committed and pushed
independently. A module owns its own command schema, Lambda folders, error
mappings, Terraform, and docs.

Module discovery for build, route validation, and command registration is
filesystem-based from `modules/*/module.manifest.json`.

This page documents the framework-side contract. Module-specific commands,
storage models, external API rules, and error-code expectations belong in
that module's own `README.md` — read it before touching module code.

## Module contract

Each installed module should provide:

- `README.md` for module users.
- `AGENTS.md` and `CLAUDE.md` for module-specific agent/developer rules.
- `module.manifest.json` with module name, command schema path, error schema
  path, route maps, error mapper Lambda, Lambda folders, and Terraform path.
- `discord.commands.json` with the module's Discord application commands.
- `errors.json` with safe Discord-facing responses for module error codes.
- `lambdas/` with module-owned command/component/modal/autocomplete/error
  Lambdas.
- `terraform/` when the module owns AWS infrastructure.

Command/component/modal Lambdas must read `application_id` and `token` from
the raw interaction payload and PATCH the original deferred response for all
success and safe-error cases — see
[the deferred-ACK / PATCH contract](architecture.md#the-deferred-ack-patch-contract).

## Manifest routes

A manifest declares routes per kind: `commands`, `user_commands`,
`message_commands`, `components`, `modals`, and `autocomplete`.

### Ephemeral deferred ACKs

A command route value may be either a plain Lambda-name string or an object
form that opts the command into an ephemeral deferred ACK:

```json
"routes": {
  "commands": {
    "account list": "discord-cmd-account-list",
    "account link": { "lambda": "discord-cmd-account-link", "ephemeral_defer": true }
  }
}
```

The plain-string form means `ephemeral_defer: false`. The object form
requires a non-empty string `"lambda"`; `"ephemeral_defer"` must be a boolean
and is legal on `commands`, `user_commands`, and `message_commands` routes —
validation rejects it on every other route kind. Slash commands opt in by
their full command path; context menu commands opt in by their raw command
name. Entries are matched exactly (`account` never matches `account link`).

Rerun `scripts/generate-terraform-modules.py` after changing any
`ephemeral_defer` flag — the opted-in paths are merged into generated
Terraform and wired into the ingress Lambda's env, never hand-maintained.

### Route-map overrides

Each router derives its worker Lambda name mechanically. A manifest route may
instead point at a non-mechanical target Lambda name; the generated env route
maps (`DISCORD_COMMAND_ROUTES` and friends — see
[Environment Variables](environment-variables.md#router-route-map-overrides))
carry these overrides to the routers, which fall back to the mechanical name
when a key is absent.

Validation treats a target that matches the mechanical derivation as
load-bearing (its folder must exist — error if missing), and a target that
differs as a supported override (a warning to ensure it is deployed and
IAM-granted, not an error). Terraform grants the routers
`lambda:InvokeFunction` on every manifest route target, so an override name
still resolves at runtime.

### Context menu commands

Context menu commands (interaction `data.type` 2 = user, 3 = message) follow
the same mechanical rule applied to the raw command name: ASCII letters
lowercased, spaces → `-` (`"Report User"` → `discord-usercmd-report-user`;
message commands → `discord-msgcmd-<name>`). Module manifests declare them
under the `user_commands` / `message_commands` route kinds.

## Adding a new command Lambda

1. Choose the owning module, e.g. `modules/<name>`.
2. Create a new folder, e.g.
   `modules/<name>/lambdas/commands/discord-cmd-<group>-<subcommand>/`
   (or scaffold it — see below).
3. Add a `main.cpp` that reads `application_id` and `token` from the
   interaction JSON, calls the desired API, then PATCHes the deferred
   response.
4. Add the subcommand to the module's `discord.commands.json`.
5. Add the route and Lambda folder to the module's `module.manifest.json`.
6. Build with `scripts/build-lambda.sh modules/<name>/lambdas/commands/...`.
7. Add module Terraform for any new infrastructure.

**The naming convention is load-bearing:** the application-command handler
derives the Lambda name mechanically from the command path
(`<group> <subcommand>` → `discord-cmd-<group>-<subcommand>`). The name must
match exactly unless you declare an explicit override.

## Scaffolding a new Lambda

`scripts/new-lambda.py` generates a correctly named Lambda skeleton inside a
module, wires its `module.manifest.json` route, appends a schema stub for
command kinds, and prints a next-step checklist. The generated `main.cpp`
already follows the framework rules (value-init with `{}`,
`application_id`/`token` via `discord_interactions::metadata`, deferred PATCH
via `patch_original_response` on success and safe-error paths, `MODULE_ERROR`
for expected failures, internals logged to stderr only). Autocomplete instead
returns the synchronous `{type:8}` choice payload via
`autocomplete_response`.

```bash
# Slash command (path is "group sub" / "group subgroup sub")
python3 scripts/new-lambda.py --module <name> --kind command --path "account link"

# Message component / modal (path is the custom_id prefix, a single token)
python3 scripts/new-lambda.py --module <name> --kind component --path vote
python3 scripts/new-lambda.py --module <name> --kind modal --path feedback

# Autocomplete (requires --option)
python3 scripts/new-lambda.py --module <name> --kind autocomplete \
    --path weather --option city

# Preview without writing anything
python3 scripts/new-lambda.py --module <name> --kind command --path "account link" --dry-run
```

The generator refuses (non-zero exit) if the route already exists. After
scaffolding, implement the `TODO`, build with `scripts/build-lambda.sh`, and
register with `scripts/register-discord-commands.py`.
