# Environment Variables

## `discord-interactions` (ingress Lambda)

| Variable | Required | Description |
|---|---|---|
| `DISCORD_PUBLIC_KEY` | Yes | Hex-encoded Ed25519 public key from the Discord developer portal |
| `AWS_REGION` | Yes | Region for the AWS SDK Lambda client |
| `AWS_LAMBDA_ENDPOINT` | No | Override Lambda endpoint (used in local tests to point at the mock server) |
| `DISCORD_SKIP_SIGNATURE_VERIFY` | No | Set to `1` to bypass signature verification (local testing only) |
| `DISCORD_EPHEMERAL_DEFER_ROUTES` | No | Generated comma-separated allowlist of command paths whose type-2 deferred ACK is ephemeral (`{"type":5,"data":{"flags":64}}`). Emitted into generated Terraform by `scripts/generate-terraform-modules.py` from manifest `ephemeral_defer` flags — never hand-maintained. Entries are whitespace-trimmed and matched exactly against the full command path (`account` never matches `account link`); context menu commands are matched against the raw command name. Unset or empty ⇒ every command defers with the plain `{"type":5}`. |

## Handler/router Lambdas

| Variable | Required | Description |
|---|---|---|
| `AWS_REGION` | Yes | Region for the AWS SDK Lambda client |
| `AWS_LAMBDA_ENDPOINT` | No | Override Lambda endpoint (local testing) |
| `DISCORD_IDEMPOTENCY_TABLE` | No | DynamoDB table name for durable cross-container interaction dedup. Unset or empty ⇒ both `idempotency_store.hpp` primitives no-op to "proceed". Opt-in per worker Lambda; provision the table via the root `discord_idempotency_table_enabled` Terraform variable. |
| `AWS_DYNAMODB_ENDPOINT` | No | Override DynamoDB endpoint for the `idempotency_store.hpp` warm-global client (local testing — mirrors `AWS_LAMBDA_ENDPOINT`; the `dedup` suite points it at the mock server) |

## Router route-map overrides

Each router derives its worker Lambda name mechanically (see
[Architecture](architecture.md#interaction-routing)). Optionally, an env var
holding a JSON object can override that derivation per key; when unset,
empty, or missing the key, the router falls back to the mechanical name.
These are how the framework wires generated per-module route maps into the
routers. The **modal** and **autocomplete** routers have no such override —
they always derive mechanically.

| Variable | Router | Key | Description |
|---|---|---|---|
| `DISCORD_COMMAND_ROUTES` | application-command | full command path (`account link`) | JSON object mapping a slash-command path to its worker Lambda; falls back to `discord-cmd-<path>`. |
| `DISCORD_USER_COMMAND_ROUTES` | application-command | raw command name (`Report User`) | JSON object mapping a user context-menu command name to its worker Lambda; falls back to `discord-usercmd-<name>`. |
| `DISCORD_MESSAGE_COMMAND_ROUTES` | application-command | raw command name | JSON object mapping a message context-menu command name to its worker Lambda; falls back to `discord-msgcmd-<name>`. |
| `DISCORD_COMPONENT_ROUTES` | message-component | component `custom_id` prefix (text before the first `:`) | JSON object mapping a component prefix to its worker Lambda; falls back to `discord-component-<prefix>`. |

## Discord REST helpers

| Variable | Required | Description |
|---|---|---|
| `DISCORD_API_VERSION` | No | Discord REST API version. Defaults to `10`, which is currently the latest available version in Discord's official API reference. |
| `DISCORD_API_BASE_URL` | No | Full Discord REST API base URL override, e.g. `http://localhost:19001/api/v10` for local tests. Takes precedence over `DISCORD_API_VERSION`. |
