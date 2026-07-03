# Lambdacord

A C++ Discord interactions framework for AWS Lambda on `provided.al2023`
(arm64 by default). Discord sends interactions over HTTPS; a small set of
gateway Lambdas verify signatures and route by interaction type; per-module
worker Lambdas do the work. No gateway websocket, no always-on server.

The framework is a set of header-only helpers
(`src/include/discord_interactions/`) compiled into each Lambda, covering
ingress and routing, rate-limit-aware Discord REST, response builders
(embeds, components, Components V2, modals, autocomplete, pagination),
structured `custom_id` state, structured module errors, and opt-in
DynamoDB-backed interaction dedup.

Feature modules are locally installed repos under `modules/<name>/` that
bring their own commands, Lambdas, error mappings, and Terraform. This root
repo owns only the generic framework; read `modules/<name>/README.md` for a
module's behavior.

The project's design principles live in [`SOUL.md`](SOUL.md).

## Requirements

- Docker with `buildx`
- Python 3 for command registration and local test tooling
- Terraform for AWS deployment

No local C++ compiler is required — all C++ builds run inside an Amazon
Linux 2023 Docker builder image.

## Quickstart

Build every framework and installed-module Lambda
(zips land in `packaged-lambdas/`):

```bash
scripts/build-all-lambdas.sh
```

Run the fast unit tests:

```bash
scripts/test-unit.sh                  # C++ (doctest), inside the builder image
python3 -m pytest tests/unit/python   # Python (pytest), no Docker
```

Run the full local integration suites (RIE containers + mock control plane):

```bash
scripts/test-local-discord-lambdas.sh
```

Register your commands with Discord (guild-scoped is instant — use it while
iterating; omit `DISCORD_GUILD_ID` for global):

```bash
DISCORD_BOT_TOKEN="..." DISCORD_APPLICATION_ID="..." DISCORD_GUILD_ID="..." \
python3 scripts/register-discord-commands.py
```

Deploy with Terraform (regenerates module wiring, builds zips, then applies;
the stack outputs the Function URL to paste into the Discord Developer
Portal as the Interactions Endpoint URL):

```bash
scripts/terraform-deploy.sh apply
```

Scaffold a new module command Lambda:

```bash
python3 scripts/new-lambda.py --module <name> --kind command --path "account link"
```

## Documentation

Everything beyond this quickstart lives in the wiki under `docs/`, built
with [MkDocs](https://www.mkdocs.org/):

```bash
pip install mkdocs
mkdocs serve        # browse at http://127.0.0.1:8000
```

The pages are plain Markdown, so they also read fine directly in a browser
or editor:

| Page | What's there |
|---|---|
| [Philosophy](SOUL.md) | The design principles the whole repo follows. |
| [Getting Started](docs/getting-started.md) | Requirements, building Lambdas, running tests. |
| [Architecture](docs/architecture.md) | Repo layout, gateway Lambdas, interaction routing, the deferred-ACK/PATCH contract. |
| [Modules](docs/modules.md) | The module contract, manifests, routes, scaffolding. |
| [Command Registration](docs/command-registration.md) | Registering, validating, and removing Discord commands. |
| [Deployment](docs/deployment.md) | Terraform layout, generated wiring, the deploy wrapper. |
| [Testing](docs/testing.md) | Unit suites, the integration harness, the mock server API, CI. |
| [Environment Variables](docs/environment-variables.md) | Every env var the framework reads. |
| [Errors & Idempotency](docs/error-handling.md) | Structured errors, error categories, interaction dedup. |
| [Scripts Reference](docs/scripts.md) | Every script, user-facing and internal. |
| [Framework API Reference](docs/framework-reference.md) | Per-header reference for `src/include/discord_interactions/`. |

Agent/developer workflow rules are in [`AGENTS.md`](AGENTS.md)
(`CLAUDE.md` is a symlink to it).

## The one rule to know

Worker Lambdas are invoked asynchronously after Discord has already received
a deferred acknowledgement. A worker's return value is never shown to the
user — anything user-visible must be sent by PATCHing the original
interaction response:

```text
PATCH /webhooks/<application_id>/<interaction_token>/messages/@original
```

See [Architecture](docs/architecture.md) for the full contract.

## Conventions

- C++17, Lambda runtime `provided.al2023`, default architecture `arm64`.
- Generic Discord behavior belongs under `src/`; module-specific commands,
  storage, external APIs, error mappings, and Terraform belong under
  `modules/<name>/`.
- Never send raw exception text, upstream API bodies, provider errors, or
  internal debug context to Discord users.
