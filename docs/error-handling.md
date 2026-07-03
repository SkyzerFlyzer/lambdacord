# Errors & Idempotency

## Structured module errors

The framework uses structured module errors instead of exception-text
matching. Core categories are generic, while error codes are namespaced
strings owned by modules.

Module command/component Lambdas should return or throw structured errors
with:

- `code`
- `category`
- `internal_message`
- `function`
- `safe_context`
- `debug_context`

On module failure, module code should map the structured error to a safe
Discord payload and PATCH the original deferred response. Error-mapper
Lambdas may return safe payloads to internal callers, but user-visible output
still has to reach Discord through the original-response PATCH flow.

## Branch on the error category

Workers must branch on `ModuleError.category` before treating a caught error
as deterministic:

- Only the **deterministic** categories (`validation`, `auth`) warrant a
  friendly PATCH plus a success return.
- The **transient** categories (`upstream`, `storage`, `configuration`,
  `rate_limited`, `internal` — e.g. `patch_original_response` throwing
  `ErrorCategory::upstream` on a 429/5xx blip) must return a Lambda failure
  so AWS async retry re-runs instead of burning the interaction on a
  permanent "Something went wrong".

## Never leak internals

Never pass raw internal exception text, upstream API bodies, AWS SDK errors,
provider errors, or `ex.what()` directly to Discord users or browser-facing
pages. Log internal details to stderr/CloudWatch, then map expected
validation cases through module-owned structured error mappings or another
explicit allowlist. For infrastructure, Discord API, external API, storage,
JSON parsing, or curl failures, send a short friendly retry/action message
instead.

Validate user-controlled Discord command options before calling storage/API
helpers. In particular, parse numeric IDs at the command boundary and return
friendly validation copy instead of relying on helper exceptions such as
`std::stoll`.

## Interaction idempotency

AWS async invocation is at-least-once and retries failed runs, so a worker
Lambda can receive the same interaction twice. The framework's default
posture is the **completion marker, not a claim** (AD-9): record success only
after the user-visible PATCH, so duplicates of a success skip while retries
of a crash correctly re-run and the user always gets a response.

### In-process guard (same warm container)

`CompletedInteractions` (`idempotency.hpp`) guards against re-doing work that
already completed on the same warm container. `was_completed` is a const,
side-effect-free check, and `mark_completed` records an id with true LRU
eviction.

Usage contract — **check → act → PATCH → mark**:

1. `if (completed.was_completed(id)) return;` — skip already-finished work.
2. Do the work and PATCH `@original` — the user-visible effect.
3. `completed.mark_completed(id);` — **only after** the PATCH succeeded.

Never `mark_completed` before the user-visible effect has happened: marking
first means a crash before the PATCH leaves the user stuck on "thinking…"
because the retry would see the marker and skip it.

Scope honestly: this catches duplicates on the **same warm container** only;
retries minutes later often land on a cold container with an empty guard. Do
not wire this into router Lambdas — workers own the decision.

### Durable cross-container dedup

Durable dedup lives in `idempotency_store.hpp` (pure request shaping in
`idempotency_requests.hpp`). It is DynamoDB-backed and gated on
`DISCORD_IDEMPOTENCY_TABLE`: an empty table name makes every primitive a
no-op that returns "proceed", and any DynamoDB/infra error is **fail-open**
(logged to stderr, then proceed) — a duplicate PATCH beats a silently dropped
interaction.

Two primitives:

- **Completion marker (DEFAULT):** `was_completed(client, table, id)`
  (GetItem) → act → PATCH →
  `record_completion(client, table, id, now_epoch_s)` (unconditional PutItem,
  **only after** the PATCH). A retry of a run that crashed before the PATCH
  finds no record and re-runs, so the user always gets a response; duplicates
  of a success skip. Use this unless a side effect must not repeat.
- **Claim (OPT-IN):** `claim_interaction(client, table, id, now_epoch_s)`
  (conditional PutItem `attribute_not_exists(interaction_id)`) **before**
  acting; returns `false` only on `ConditionalCheckFailedException`.
  Guarantees at-most-once, but a crash after the claim suppresses the retry
  and the user may get **no** response. Reserve it for non-idempotent side
  effects (currency, purchases, irreversible external commands) where a
  dropped response is the lesser evil.

Construct the `Aws::DynamoDB::DynamoDBClient` **once as a warm global** in
`main()` (same pattern as `lambda_client.hpp`), never per invocation; point
it at `AWS_DYNAMODB_ENDPOINT` for local testing. TTL defaults to 3600 s
(interaction tokens expire at 15 min; 1 h leaves audit slack), and the
`expires_at` attribute lets DynamoDB expire records automatically.
