# Architecture

## Repository layout

```text
src/
  include/discord_interactions/  # generic Discord interaction helpers (header-only)
  lambdas/                       # framework gateway/router Lambdas

modules/<name>/                  # locally installed module repos, ignored by parent Git
  README.md                      # module user docs
  AGENTS.md / CLAUDE.md          # module agent/developer instructions
  module.manifest.json           # routes, Lambda folders, error mapper, Terraform path
  discord.commands.json          # Discord command schema contributed by the module
  errors.json                    # safe error-response mappings
  lambdas/                       # module-owned command/component/error Lambdas
  terraform/                     # module-owned infrastructure

infra/terraform/                 # root orchestration, calls installed module Terraform
scripts/                         # user entrypoints plus helper scripts
docs/                            # this wiki (mkdocs)
tests/                           # C++/Python unit suites + local integration harness
packaged-lambdas/                # generated zip artifacts
```

The framework is entirely source-level: the headers under
`src/include/discord_interactions/` are compiled into each Lambda's single
`main.cpp`. There is no shared library and no linked framework object. The
full per-header contract is in the
[Framework API Reference](framework-reference.md).

## Framework Lambdas

The generic Discord flow is split into gateway/router Lambdas under
`src/lambdas`:

| Lambda folder | Zip name | Purpose |
|---|---|---|
| `src/lambdas/discord-interactions` | `discord-interactions.zip` | Ingress gateway — verifies Discord Ed25519 signatures, handles Discord `PING`, and dispatches by interaction type. |
| `src/lambdas/discord-application-command-handler` | `discord-application-command-handler.zip` | Routes slash commands and context menu commands through the merged module command route maps. |
| `src/lambdas/discord-message-component-handler` | `discord-message-component-handler.zip` | Routes component interactions through the merged module component route map. |
| `src/lambdas/discord-modal-handler` | `discord-modal-handler.zip` | Routes modal submits through the merged module modal route map. |
| `src/lambdas/discord-autocomplete-handler` | `discord-autocomplete-handler.zip` | Routes autocomplete interactions (synchronously) through the merged module autocomplete route map. |

## Interaction routing

```text
Discord HTTP POST
  └─► discord-interactions          (verify sig, dispatch by type)
        ├─ type 1 (PING)            → respond {type:1} inline
        ├─ type 2 (APPLICATION_CMD) → async ► discord-application-command-handler
        │                                         ├─ data.type 1/absent (slash)   ─► discord-cmd-<path>
        │                                         ├─ data.type 2 (user ctx menu)  ─► discord-usercmd-<name>
        │                                         └─ data.type 3 (msg ctx menu)   ─► discord-msgcmd-<name>
        ├─ type 3 (MESSAGE_COMP)    → async ► discord-message-component-handler
        │                                         └─► discord-component-<prefix>
        ├─ type 4 (AUTOCOMPLETE)    → sync  ► discord-autocomplete-handler
        │                                         └─► discord-autocomplete-<path>-<option>
        └─ type 5 (MODAL_SUBMIT)    → async ► discord-modal-handler
                                              └─► discord-modal-<prefix>
```

Worker Lambda names are derived **mechanically** from the interaction —
`account link` routes to `discord-cmd-account-link`, a component `custom_id`
of `vote:...` routes to `discord-component-vote`. The naming convention is
load-bearing; explicit per-key overrides are supported through generated env
route maps (see [Modules](modules.md#route-map-overrides)).

## The deferred-ACK / PATCH contract

The framework invokes downstream command/component/modal worker Lambdas
**asynchronously**, after the ingress has already sent Discord a deferred
acknowledgement. Because of that, a worker Lambda's `invocation_response` is
only runtime bookkeeping — Discord never sees it.

Any user-visible command result, component update, or safe error message must
be sent by PATCHing the original interaction response:

```text
PATCH /webhooks/<application_id>/<interaction_token>/messages/@original
```

Worker Lambdas are expected to:

1. PATCH the original interaction response for every success and safe error
   path.
2. Return `{"ok":true}` to the Lambda runtime only after the PATCH succeeds.
3. Log internal failures and return a Lambda failure only for runtime
   observability (which also lets AWS async retry re-run transient failures).

Do not return a Discord interaction response payload (`{"type": 4, ...}`)
from a worker Lambda and expect Discord to render it.

## Ephemeral deferred ACKs

A command can opt in to an **ephemeral** deferred ACK (the "thinking…" state
only the invoking user can see) by declaring `"ephemeral_defer": true` on its
manifest route. `scripts/generate-terraform-modules.py` merges every
installed module's opt-ins into the ingress Lambda's
`DISCORD_EPHEMERAL_DEFER_ROUTES` env var through the generated Terraform —
the ingress only parses the env var and never reads manifests at runtime.
Opted-in type-2 interactions get `{"type":5,"data":{"flags":64}}` instead of
the plain `{"type":5}`; component and modal ACKs are unchanged. See
[Modules](modules.md#ephemeral-deferred-acks) for the manifest form.

Ephemerality is fixed by the ACK: Discord ignores `flags` on webhook message
edits, so the later `@original` PATCH inherits the visibility chosen at defer
time.

## Unknown-route handling

When a router resolves a worker Lambda name that does not exist, the AWS SDK
`Invoke` fails with `ResourceNotFoundException`. Rather than leave the user
hanging on "thinking…" / "application did not respond", the routers reply
cleanly:

- The three **async** routers (application-command, message-component, modal)
  PATCH `@original` with a shared friendly message (never containing internal
  error text), log the real SDK error to stderr, then still return a failed
  invocation for observability. The reply is posted with the visibility of
  the original deferred ACK.
- The **sync** autocomplete router does not PATCH (it is on Discord's
  3-second budget); it returns an empty result
  `{"type":8,"data":{"choices":[]}}` so Discord simply shows no suggestions.

Every other invoke failure keeps the default behavior: log plus a generic
Lambda failure.
