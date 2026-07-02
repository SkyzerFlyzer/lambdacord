# AGENTS.md

## Project Overview

This repo builds AWS Lambda functions in C++ targeting `provided.al2023` (arm64 by default). The main repo owns the generic Discord interactions framework under `src/`. Feature modules live under `modules/<name>/` and bring their own command schemas, command/component/error Lambdas, error mappings, and Terraform. The parent repo ignores `modules/`; module repos are installed locally and committed/pushed independently.

Module-specific instructions live in each module's own `AGENTS.md` and `CLAUDE.md`. Read those before changing module code.

---

## Build System

All builds run inside Docker using an Amazon Linux 2023 builder image. There is no local compiler requirement.

**Build a single Lambda:**
```bash
scripts/build-lambda.sh <lambda-folder>
```

**Build the full deployed Lambda set:**
```bash
scripts/build-all-lambdas.sh
```

**Build for x86_64 instead of arm64:**
```bash
LAMBDA_ARCH=x86_64 scripts/build-lambda.sh <lambda-folder>
```

**Reuse an existing builder image (skip the `docker buildx build` step):**
```bash
LAMBDA_SKIP_IMAGE_BUILD=1 scripts/build-lambda.sh <lambda-folder>
```

Output zips land in `packaged-lambdas/<lambda-name>.zip`. The `.gitignore` excludes per-Lambda `dist/` and `.lambda-build/` scratch directories.

**The build script auto-generates a `CMakeLists.txt`** from all `*.cpp`/`*.cc`/`*.cxx` files found in the Lambda folder. Drop a `CMakeLists.txt` into the folder to override this behaviour (the script will use it directly).

### Dependencies baked into the builder image (`docker/lambda-builder/Dockerfile`)

| Library | Source |
|---|---|
| `aws-lambda-cpp` | Built from source (awslabs/aws-lambda-cpp) |
| `aws-sdk-cpp` (Lambda client only) | Built from source |
| `libsodium` | Amazon Linux 2023 DNF |
| `libcurl` | Amazon Linux 2023 DNF |
| `nlohmann/json` | Fetched via CMake `FetchContent` at build time |

`libsodium` is dynamically linked and bundled into every zip alongside `bootstrap`.

---

## Script Ownership

User-facing entrypoints are documented in `README.md`. Agent/helper scripts are:

| Script | Intended use |
|---|---|
| `scripts/build-lambda-in-docker.sh` | Internal build implementation called by `scripts/build-lambda.sh` inside the builder container. Do not ask users to call it directly. |
| `scripts/lib/discord_modules.py` | Shared Python helper for module manifest discovery, route merging, schema merging, and validation. Import from user-facing scripts instead of duplicating manifest parsing. |
| `scripts/generate-terraform-modules.py` | Generates root Terraform module calls, pass-through variables, manifest locals, and module output proxies from installed module manifests. Run after adding/removing module Terraform. |
| `scripts/pre-commit-static-checks.sh` | Hook/agent static-analysis runner. Users may run it manually, but the README points them at `scripts/install-git-hooks.sh` first. |
| `scripts/mock-lambda-runtime-api.py` | Local/debug helper for Lambda runtime experiments, not part of the normal deploy flow. |
| `tests/local/discord/run_local_tests.py` | Test harness behind `scripts/test-local-discord-lambdas.sh`; call it directly only when selecting suites during development. |
| `tests/local/discord/mock_lambda_server.py` | Local Lambda control-plane mock used by the test harness. |

Keep this section updated whenever adding helper scripts. If a script is meant
for normal user workflows, document it in `README.md` instead.

Generated Terraform files live under `infra/terraform/generated_*.tf`. Do not
edit those files by hand; update module manifests/module Terraform and rerun
`scripts/generate-terraform-modules.py`.

---

## Lambda Catalogue

### Discord gateway stack

| Lambda folder | Zip name | Purpose |
|---|---|---|
| `src/lambdas/discord-interactions` | `discord-interactions.zip` | Ingress — verifies Ed25519 signatures, dispatches by interaction type |
| `src/lambdas/discord-application-command-handler` | `discord-application-command-handler.zip` | Routes slash commands → `discord-cmd-<path>` (async) |
| `src/lambdas/discord-message-component-handler` | `discord-message-component-handler.zip` | Routes component interactions → `discord-component-<prefix>` (async) |
| `src/lambdas/discord-modal-handler` | `discord-modal-handler.zip` | Routes modal submits → `discord-modal-<prefix>` (async) |
| `src/lambdas/discord-autocomplete-handler` | `discord-autocomplete-handler.zip` | Routes autocomplete → `discord-autocomplete-<path>-<option>` (sync) |

### Modules

Modules live under `modules/<name>/` and are locally installed repos. Do not document
module-specific commands, APIs, storage models, or Terraform behavior in this
root file. Read and update the module's own `AGENTS.md` and `CLAUDE.md` before
changing module code.

### Adding a new command Lambda

1. Choose the owning module, e.g. `modules/<name>`.
2. Create a new folder, e.g. `modules/<name>/lambdas/commands/discord-cmd-<group>-<subcommand>/`.
3. Add a `main.cpp` that reads `application_id` and `token` from the interaction JSON, calls the desired API, then PATCHes the deferred response via `discord_patch_original`.
4. Add the subcommand to the module's `discord.commands.json`.
5. Add the route and Lambda folder to the module's `module.manifest.json`.
6. Build with `scripts/build-lambda.sh modules/<name>/lambdas/commands/discord-cmd-<group>-<subcommand>`.
7. Add module Terraform for any new infrastructure.

**Naming convention is load-bearing:** the application-command handler derives the Lambda name mechanically from the command path (`<group> <subcommand>` → `discord-cmd-<group>-<subcommand>`). The name must match exactly.

---

## Interaction Routing

```
Discord HTTP POST
  └─► discord-interactions          (verify sig, dispatch by type)
        ├─ type 1 (PING)            → respond {type:1} inline
        ├─ type 2 (APPLICATION_CMD) → async ► discord-application-command-handler
        │                                         └─► discord-cmd-<path>
        ├─ type 3 (MESSAGE_COMP)    → async ► discord-message-component-handler
        │                                         └─► discord-component-<prefix>
        ├─ type 4 (AUTOCOMPLETE)    → sync  ► discord-autocomplete-handler
        │                                         └─► discord-autocomplete-<path>-<option>
        └─ type 5 (MODAL_SUBMIT)    → async ► discord-modal-handler
                                              └─► discord-modal-<prefix>
```

Command Lambdas receive the raw interaction JSON. They are invoked
asynchronously after the gateway has already sent a deferred ACK to Discord.
Their Lambda `invocation_response` is not rendered by Discord.

They are expected to:
1. PATCH the original interaction response via `https://discord.com/api/v<DISCORD_API_VERSION>/webhooks/<application_id>/<token>/messages/@original` for every success and safe error path.
2. Return `{"ok":true}` to the Lambda runtime only after the PATCH succeeds.
3. Log internal failures and return a Lambda failure only for runtime observability.

Never return a Discord interaction response payload from an async command,
component, or modal worker Lambda and expect Discord to show it. If the user
should see it, PATCH `@original`.

---

## Environment Variables

### `discord-interactions` (ingress Lambda)

| Variable | Required | Description |
|---|---|---|
| `DISCORD_PUBLIC_KEY` | Yes | Hex-encoded Ed25519 public key from the Discord developer portal |
| `AWS_REGION` | Yes | Region for the AWS SDK Lambda client |
| `AWS_LAMBDA_ENDPOINT` | No | Override Lambda endpoint (used in local tests to point at the mock server) |
| `DISCORD_SKIP_SIGNATURE_VERIFY` | No | Set to `1` to bypass signature verification (local testing only) |

### Handler/router Lambdas

| Variable | Required | Description |
|---|---|---|
| `AWS_REGION` | Yes | Region for the AWS SDK Lambda client |
| `AWS_LAMBDA_ENDPOINT` | No | Override Lambda endpoint (local testing) |

### Discord REST helpers

| Variable | Required | Description |
|---|---|---|
| `DISCORD_API_VERSION` | No | Discord REST API version. Defaults to `10`, which is currently the latest available version in Discord's official API reference. |
| `DISCORD_API_BASE_URL` | No | Full Discord REST API base URL override, e.g. `http://localhost:19001/api/v10` for local tests. Takes precedence over `DISCORD_API_VERSION`. |

---

## Local Testing

Fast Python unit tests (no Docker): `python3 -m pytest tests/unit/python` runs the Python unit layer for `scripts/lib`.

Tests require Docker and Python 3. They spin up real Lambda containers via the AWS Lambda Runtime Interface Emulator (RIE) and a lightweight Python mock server that stands in for the Lambda control plane.

```bash
scripts/test-local-discord-lambdas.sh
```

This calls `tests/local/discord/run_local_tests.py`, which:
1. Verifies Docker is available and the RIE image can run on `linux/arm64`.
2. Starts `tests/local/discord/mock_lambda_server.py` on port `19001`.
3. Extracts each zip from `packaged-lambdas/` into a temp directory and mounts it as `/var/runtime` inside an RIE container.
4. Runs test suites: ingress, application-command routing, component routing, modal routing, autocomplete routing.
5. Tears everything down and prints `All local Discord Lambda tests passed.` on success.

**All zips must be built before running tests.** Build them all first:
```bash
scripts/build-all-lambdas.sh
```

### Mock server API

The mock server (`tests/local/discord/mock_lambda_server.py`) exposes:

- `POST /__reset` — reset logs and configure canned responses: `{"responses": {"function-name": <payload>}}`
- `GET /__logs` — retrieve the list of recorded invocations: `[{function_name, invocation_type, payload}]`
- `POST /2015-03-31/functions/<name>/invocations` — Lambda-style invocation endpoint

---

## Registering Discord Commands

```bash
# Guild-scoped (instant, use while iterating)
DISCORD_BOT_TOKEN="..." \
DISCORD_APPLICATION_ID="..." \
DISCORD_GUILD_ID="..." \
python3 scripts/register-discord-commands.py

# Global
DISCORD_BOT_TOKEN="..." \
DISCORD_APPLICATION_ID="..." \
python3 scripts/register-discord-commands.py
```

By default, `scripts/register-discord-commands.py` discovers every `modules/*/module.manifest.json` file and merges the installed module command schemas before sending a Discord bulk overwrite. The registration script validates module manifests, route maps, command schemas, Lambda folders, error schemas, and Terraform folders. Use `--validate-only` for a local consistency check, pass `--commands-file <path>` only when intentionally registering a custom schema, or `--skip-validation` if you intentionally want a partial manifest. To clear commands entirely, use `scripts/remove-discord-commands.py` for either guild-scoped removal or `--global` removal.

Copy `.env.example` to `.env` and fill in real values for convenience.

### Module Behavior

Module-specific account models, external API rules, command semantics, and
error-code expectations belong in that module's own docs. Keep the root docs
focused on the framework contract.

---

## Code Style & Conventions

- **C++17**, compiled with `-Os -flto -ffunction-sections -fdata-sections`.
- Always value-initialize C++ locals, globals, structs, SDK request/config
  objects, C API status/output variables, JSON payloads, strings, streams, and
  containers with `{}` or an explicit value. Do not rely on architecture,
  compiler, or library defaults for zero-initialization; this is especially
  important for arm64/aarch64 builds and C/C++ API boundary objects.
- Each Lambda is a single `main.cpp`. Shared helpers (e.g. `write_callback`, `http_get_json`, `discord_patch_original`) are currently duplicated per-Lambda inside an anonymous namespace — extract to a shared `include/` header if the duplication grows unwieldy.
- Handler Lambdas that do not call downstream Lambdas normally link only against `aws-lambda-cpp` and their own direct dependencies; they do not need the AWS SDK Lambda client.
- Handler Lambdas that invoke other Lambdas use the AWS SDK `LambdaClient` initialised once in `main()` as a global, following the standard warm-start pattern.
- Discord embed fields must stay under Discord's limits — use `safe_truncate(..., 1500)` on JSON-formatted response bodies.
- All ephemeral responses use `"flags": 64`.
- Downstream worker Lambdas must PATCH deferred Discord responses; Lambda
  return values are not user-visible in the async routing path.
- Never pass raw internal exception text, upstream API bodies, AWS SDK errors, provider errors, or `ex.what()` directly to Discord users or browser-facing pages. Log internal details to stderr/CloudWatch, then map expected validation cases through module-owned structured error mappings or another explicit allowlist. For infrastructure, Discord API, external API, storage, JSON parsing, or curl failures, send a short friendly retry/action message instead.
- Validate user-controlled Discord command options before calling storage/API helpers. In particular, parse numeric IDs at the command boundary and return friendly validation copy instead of relying on helper exceptions such as `std::stoll`.
