# Lambdacord

Lambdacord is a C++ Discord interactions framework for AWS Lambda on
`provided.al2023` (arm64 by default). Discord sends interactions over HTTPS;
a small set of gateway Lambdas verify and route them; per-module worker
Lambdas do the work. There is no gateway websocket and no always-on server.

The framework lives in `src/` as a set of header-only helpers
(`src/include/discord_interactions/`) compiled into each Lambda, plus the
gateway Lambdas that verify inbound Discord requests and route them by
interaction type. Feature modules are locally installed repos under
`modules/<name>/` that bring their own commands, Lambdas, error mappings,
and Terraform.

## What the framework covers

- **Ingress & routing** — Ed25519 signature verification, dispatch by
  interaction type, and mechanical routing of slash commands, user/message
  context menus, components, modals, and autocomplete to per-module worker
  Lambdas.
- **Rate-limit-aware REST** — a curl-based Discord REST core with 429/5xx
  retry whose wait budget is clamped to the Lambda invocation deadline (sync
  interaction paths never sleep-retry), plus the interaction-webhook message
  lifecycle (followup / edit / delete).
- **Response builders** — embeds, message components (buttons, selects,
  action rows), Components V2 layouts, modals, autocomplete choices,
  pagination, premium buttons, and Discord formatting utilities, all clamped
  to Discord's documented limits.
- **State & correctness** — a structured `custom_id` state codec (the only
  serverless state channel), typed command-option access, structured module
  errors, ephemeral deferred ACKs declared per command, and opt-in
  DynamoDB-backed interaction dedup (completion-marker default, at-most-once
  claim opt-in).
- **Developer tooling** — a scaffolding generator for new module Lambdas, a
  fast C++ (doctest) and Python (pytest) unit-test layer, and a GitHub
  Actions CI workflow that runs both plus static checks on every push and
  pull request.

## Where to go

| Page | What's there |
|---|---|
| [Philosophy](philosophy.md) | The design principles the whole repo follows (also `SOUL.md` at the repo root). |
| [Getting Started](getting-started.md) | Requirements, building Lambdas, running tests. |
| [Architecture](architecture.md) | Repository layout, the gateway Lambdas, interaction routing, and the deferred-ACK/PATCH contract. |
| [Modules](modules.md) | The module contract, manifests, routes, and scaffolding new Lambdas. |
| [Command Registration](command-registration.md) | Registering, validating, and removing Discord application commands. |
| [Deployment](deployment.md) | Terraform layout, generated wiring, and the deploy wrapper. |
| [Testing](testing.md) | Unit suites, the local integration harness, the mock server API, and CI. |
| [Environment Variables](environment-variables.md) | Every env var the framework Lambdas and REST helpers read. |
| [Errors & Idempotency](error-handling.md) | Structured module errors, error-category handling, and interaction dedup. |
| [Scripts Reference](scripts.md) | Every script in `scripts/`, user-facing and internal. |
| [Framework API Reference](framework-reference.md) | Per-header API reference for `src/include/discord_interactions/`. |

Module-specific behavior (commands, storage, external APIs, module Terraform)
is documented in each module's own `README.md` — this wiki intentionally
covers only the framework contract.
