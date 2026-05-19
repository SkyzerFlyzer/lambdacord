# Discord Interactions C++ Lambda Framework

This repository contains the main C++ Discord interactions framework for AWS
Lambda on `provided.al2023`. The framework lives in `src/` and handles generic
Discord request verification, routing, response helpers, pagination, followups,
and structured module error flow.

Feature modules are locally installed repos under `modules/<name>/`. A module owns its
own command schema, Lambda folders, error mappings, Terraform, and module docs.
The root README intentionally avoids documenting module-specific commands or
infrastructure; read `modules/<name>/README.md` for that module's behavior.

## Repository Layout

```text
src/
  include/discord_interactions/  # generic Discord interaction helpers
  lambdas/                       # framework gateway/router Lambdas

modules/<name>/                  # locally installed module repos, ignored by parent Git
  README.md                      # module user docs
  AGENTS.md                      # module agent/developer instructions
  CLAUDE.md                      # module agent/developer instructions
  module.manifest.json           # routes, Lambda folders, error mapper, Terraform path
  discord.commands.json          # Discord command schema contributed by the module
  errors.json                    # safe error-response mappings
  lambdas/                       # module-owned command/component/error Lambdas
  terraform/                     # module-owned infrastructure

infra/terraform/                 # root orchestration, calls installed module Terraform
scripts/                         # user entrypoints plus helper scripts
packaged-lambdas/                # generated zip artifacts
```

## User Scripts

These are the scripts normal project users should reach for:

| Script | Purpose |
|---|---|
| `scripts/build-lambda.sh <lambda-folder>` | Build one C++ Lambda zip from a framework or module Lambda folder. |
| `scripts/build-all-lambdas.sh` | Build every Lambda declared by the framework and installed module manifests. Run this before Terraform deploys. |
| `scripts/test-local-discord-lambdas.sh` | Run the local Discord Lambda integration suites against built zip artifacts. |
| `scripts/generate-terraform-modules.py` | Regenerate root Terraform module wiring from installed module manifests. |
| `scripts/terraform-deploy.sh <action>` | Regenerate module Terraform wiring, optionally build Lambdas, then run Terraform `init`, `validate`, `plan`, `apply`, or `output`. |
| `scripts/register-discord-commands.py` | Merge installed module command schemas and bulk overwrite Discord application commands. |
| `scripts/register-discord-commands.py --validate-only` | Validate installed module manifests, routes, command schemas, error schemas, Lambda folders, and Terraform folders without calling Discord. |
| `scripts/remove-discord-commands.py` | Clear registered Discord commands for a guild or globally. |
| `scripts/print-module-route-map.py <kind>` | Print the merged module route map for `commands`, `components`, `modals`, or `autocomplete`. |
| `scripts/install-git-hooks.sh` | Install the repository pre-commit hook locally. |

Lower-level helper scripts used by the build system, tests, hooks, or agents
are documented in `AGENTS.md`/`CLAUDE.md` rather than as normal user entrypoints.

## Requirements

- Docker with `buildx`
- Python 3 for command registration and local test tooling
- Terraform for AWS deployment

No local C++ compiler is required. All C++ builds run inside an Amazon Linux
2023 Docker builder image.

## Build

Build one Lambda:

```bash
scripts/build-lambda.sh src/lambdas/discord-interactions
scripts/build-lambda.sh modules/<name>/lambdas/<lambda-folder>
```

Build all framework and installed module Lambdas:

```bash
scripts/build-all-lambdas.sh
```

Build for x86_64 instead of the default arm64:

```bash
LAMBDA_ARCH=x86_64 scripts/build-lambda.sh modules/<name>/lambdas/<lambda-folder>
```

Reuse an existing builder image:

```bash
LAMBDA_SKIP_IMAGE_BUILD=1 scripts/build-lambda.sh modules/<name>/lambdas/<lambda-folder>
```

Zip artifacts are written to `packaged-lambdas/<lambda-name>.zip`.

The build script auto-generates a temporary CMake project from all `*.cpp`,
`*.cc`, and `*.cxx` files found in the Lambda folder. If a Lambda folder
contains its own `CMakeLists.txt`, the build script uses that instead.

The builder image includes:

| Library | Source |
|---|---|
| `aws-lambda-cpp` | Built from source |
| `aws-sdk-cpp` | Built from source with the clients this project needs |
| `libsodium` | Amazon Linux 2023 DNF |
| `libcurl` | Amazon Linux 2023 DNF |
| `nlohmann/json` | CMake `FetchContent` |

## Framework Lambdas

The generic Discord flow is split into framework Lambdas under `src/lambdas`:

| Lambda folder | Zip name | Purpose |
|---|---|---|
| `src/lambdas/discord-interactions` | `discord-interactions.zip` | Ingress gateway, verifies Discord Ed25519 signatures, handles Discord `PING`, and dispatches by interaction type. |
| `src/lambdas/discord-application-command-handler` | `discord-application-command-handler.zip` | Routes slash commands through the merged module command route map. |
| `src/lambdas/discord-message-component-handler` | `discord-message-component-handler.zip` | Routes component interactions through the merged module component route map. |
| `src/lambdas/discord-modal-handler` | `discord-modal-handler.zip` | Routes modal submits through the merged module modal route map. |
| `src/lambdas/discord-autocomplete-handler` | `discord-autocomplete-handler.zip` | Routes autocomplete interactions through the merged module autocomplete route map. |

Interaction routing:

```text
Discord HTTP POST
  -> discord-interactions
       type 1 PING            -> inline PONG
       type 2 command         -> discord-application-command-handler -> module command Lambda
       type 3 component       -> discord-message-component-handler   -> module component Lambda
       type 4 autocomplete    -> discord-autocomplete-handler        -> module autocomplete Lambda
       type 5 modal submit    -> discord-modal-handler               -> module modal Lambda
```

Modules contribute downstream Lambdas through `module.manifest.json`. The
framework does not need to know module-specific command names at compile time.

The framework invokes downstream command/component/modal worker Lambdas
asynchronously after sending Discord a deferred acknowledgement. Because of
that, a worker Lambda's `invocation_response` is only runtime bookkeeping and is
not sent to Discord. Any user-visible command result, component update, or safe
error message must be sent by PATCHing the original interaction response:

```text
PATCH /webhooks/<application_id>/<interaction_token>/messages/@original
```

Do not return a Discord interaction response payload from a worker Lambda and
expect Discord to render it.

## Module Contract

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

Command/component/modal Lambdas must read `application_id` and `token` from the
raw interaction payload and PATCH the original deferred response for all
success and error cases. Returning `{"type": 4, ...}` or another Discord
payload from the Lambda handler is not sufficient because downstream workers
are invoked asynchronously.

Module discovery for build, route validation, and command registration is
filesystem-based from `modules/*/module.manifest.json`.

Terraform is different: Terraform itself cannot dynamically instantiate
arbitrary module sources by scanning the filesystem. This repo handles that by
generating root Terraform wiring from installed module manifests into
`infra/terraform/generated_*.tf`; run `scripts/generate-terraform-modules.py`
directly, or use `scripts/terraform-deploy.sh`, which runs it for you.

## Discord Command Registration

Validate installed module manifests and command schemas without calling Discord:

```bash
python3 scripts/register-discord-commands.py --validate-only
```

Register guild-scoped commands while iterating:

```bash
DISCORD_BOT_TOKEN="your-bot-token" \
DISCORD_APPLICATION_ID="your-app-id" \
DISCORD_GUILD_ID="your-guild-id" \
python3 scripts/register-discord-commands.py
```

Register global commands:

```bash
DISCORD_BOT_TOKEN="your-bot-token" \
DISCORD_APPLICATION_ID="your-app-id" \
python3 scripts/register-discord-commands.py
```

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

Print the merged route map:

```bash
python3 scripts/print-module-route-map.py commands
python3 scripts/print-module-route-map.py components
```

Copy `.env.example` to `.env` if you want a local place to keep Discord command
registration values.

Discord REST calls default to API version `10`, which is currently the latest
available version in Discord's official API reference. Set
`DISCORD_API_VERSION` to use a different version, or set
`DISCORD_API_BASE_URL` to override the full API base URL for local tests.

## Terraform Deploy

Root Terraform lives in `infra/terraform`. It deploys the generic framework
infrastructure and calls each installed module's Terraform explicitly.

Build all zip artifacts first:

```bash
scripts/build-all-lambdas.sh
```

Then configure and deploy from the root stack:

```bash
cd infra/terraform
cp terraform.tfvars.example terraform.tfvars
# fill in the root variables
terraform init
terraform validate
terraform plan
terraform apply
```

Or use the wrapper script from the repo root:

```bash
scripts/terraform-deploy.sh plan
scripts/terraform-deploy.sh apply
```

The wrapper regenerates `infra/terraform/generated_*.tf` files from installed
module manifests before running Terraform. It also builds Lambda zips unless
you pass `--skip-build`.

The root stack outputs the Discord interactions Function URL. Use that URL as
the Interactions Endpoint URL in the Discord Developer Portal.

For module-specific Terraform variables, resources, and outputs, read that
module's `README.md`. Modules may provide their own `terraform.tfvars.example`
with values to copy into the root stack's `terraform.tfvars` or pass as an
additional Terraform var-file.

## Local Testing

Build all zips before running local tests:

```bash
scripts/build-all-lambdas.sh
scripts/test-local-discord-lambdas.sh
```

The test script spins up local Lambda containers via the AWS Lambda Runtime
Interface Emulator and a lightweight mock Lambda control plane.

Agents and maintainers can call `tests/local/discord/run_local_tests.py`
directly when selecting a narrower test suite; normal users should use
`scripts/test-local-discord-lambdas.sh`.

## Hooks And Static Checks

Install the versioned pre-commit hook:

```bash
scripts/install-git-hooks.sh
```

The hook runs static checks for staged C++ Lambda targets. The lower-level
static-check script is documented in `AGENTS.md`.

## Error Handling

The framework uses structured module errors instead of exception-text matching.
Core categories are generic, while error codes are namespaced strings owned by
modules.

Module command/component Lambdas should return or throw structured errors with:

- `code`
- `category`
- `internal_message`
- `function`
- `safe_context`
- `debug_context`

On module failure, module code should map the structured error to a safe
Discord payload and PATCH the original deferred response. Error-mapper Lambdas
may return safe payloads to internal callers, but user-visible output still has
to reach Discord through the original-response PATCH flow.

## Conventions

- C++17.
- Lambda runtime: `provided.al2023`.
- Default architecture: `arm64`.
- Generic Discord behavior belongs under `src/`.
- Module-specific commands, components, external API helpers, storage, error
  mappings, and Terraform belong under `modules/<name>/`.
- Never send raw exception text, upstream API bodies, provider errors, or
  internal debug context directly to Discord users.
