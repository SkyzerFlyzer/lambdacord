# Testing

Correctness is proven in layers, fastest first.

## Fast C++ unit tests (doctest)

`scripts/test-unit.sh` compiles `tests/unit/cpp/*.cpp` against `src/include`
inside the builder image (arm64 by default, like the builds — set
`LAMBDA_ARCH=x86_64` to run natively on an x86_64 host; no zip packaging, no
RIE) and runs the doctest suite; the exit code propagates.

```bash
scripts/test-unit.sh                       # run all C++ unit tests
scripts/test-unit.sh --filter 'route*'     # run a subset by doctest filter
```

Add a test by dropping `tests/unit/cpp/test_<name>.cpp` — the CMake
`file(GLOB ...)` picks it up with no CMakeLists edit. The script honors
`LAMBDA_ARCH`, `LAMBDA_BUILDER_IMAGE`, and `LAMBDA_SKIP_IMAGE_BUILD` exactly
like `scripts/build-lambda.sh`.

## Fast Python unit tests (pytest)

No Docker required:

```bash
python3 -m pytest tests/unit/python
```

This covers the `scripts/lib` helpers (manifest discovery, route merging,
schema validation), the scaffolding generator, and the mock server.

## Local integration suites

These require Docker and Python 3. They spin up real Lambda containers via
the AWS Lambda Runtime Interface Emulator (RIE) and a lightweight Python mock
server that stands in for the Lambda control plane.

**All zips must be built first:**

```bash
scripts/build-all-lambdas.sh
scripts/test-local-discord-lambdas.sh
```

This calls `tests/local/discord/run_local_tests.py`, which:

1. Verifies Docker is available and the RIE image can run on `linux/arm64`.
2. Starts `tests/local/discord/mock_lambda_server.py` on port `19001`.
3. Extracts each zip from `packaged-lambdas/` into a temp directory and
   mounts it as `/var/runtime` inside an RIE container.
4. Runs the test suites (below).
5. Tears everything down and prints
   `All local Discord Lambda tests passed.` on success.

Suites: `ingress`, `application` (application-command routing), `component`
(component routing), `modal` (modal routing), `autocomplete` (autocomplete
routing), `rest` (REST layer), `dedup` (durable interaction dedup), and
`nitrado-responses`. Select a subset with one or more `--suite <name>` flags
on `run_local_tests.py` (agents/maintainers only; normal users should use the
wrapper script). The `nitrado-responses` suite is module-specific and
self-skips (prints `SKIP`) when `modules/nitrado` is not installed; it is the
only suite that runs without the Docker/RIE control-plane mock.

### Fixture Lambdas (built on demand)

Test-only fixture Lambdas live under `tests/local/discord/fixtures/<name>/`.
They are never deployed and are deliberately **not** part of
`scripts/build-all-lambdas.sh`. Suites that need one call
`ensure_fixture_zip(fixture_dir)` in `run_local_tests.py` first, which builds
`packaged-lambdas/<name>.zip` via `scripts/build-lambda.sh` (inheriting
`LAMBDA_ARCH` / `LAMBDA_SKIP_IMAGE_BUILD` / `LAMBDA_BUILDER_IMAGE`) only when
the zip is missing or older than the fixture sources.

- The `rest` suite uses `fixtures/discord-cmd-test-echo/`, a worker that
  PATCHes `@original` via `discord_request` with the **default** retry policy
  (async workers may sleep-retry per AD-8; `patch_original_response` stays
  `no_retry` for the sync gateway paths), then creates and deletes a followup
  via `webhook_messages.hpp`. Pointed at the mock server with
  `DISCORD_API_BASE_URL`, it proves the retry loop (429-then-200 sequence →
  two recorded attempts) and the followup/delete paths against live HTTP.
- The `dedup` suite uses `fixtures/discord-cmd-test-dedup/`, a worker running
  the AD-9 completion-marker flow (`was_completed` → PATCH →
  `record_completion` from `idempotency_store.hpp`) with a warm-global
  `DynamoDBClient` pointed at the mock server via `AWS_DYNAMODB_ENDPOINT` and
  opted in via `DISCORD_IDEMPOTENCY_TABLE`. It proves that a duplicate
  delivery of a success PATCHes exactly once, that a run crashing **before**
  the PATCH records nothing so the retry re-runs and the user still gets a
  response, that distinct interaction ids stay independent, and that
  unsetting `DISCORD_IDEMPOTENCY_TABLE` opts the whole feature out. The crash
  is simulated with a test-only `test_fail_before_patch` field in the
  interaction payload (payload, not env, so one warm container serves both
  the crash and the retry).
- `fixtures/build-smoke-dynamodb/` is a manual/one-shot compile-and-link
  proof that a worker can link the DynamoDB SDK client — it is **not** wired
  into any suite or into CI; the `dedup` fixture supersedes it for the normal
  test flow.

## Mock server API

The mock server (`tests/local/discord/mock_lambda_server.py`) exposes:

- `POST /__reset` — reset all logs and configure canned behavior:
    - `{"responses": {"function-name": <payload>}}` — canned Lambda
      invocation responses
    - `{"discord_sequences": {"<METHOD> <path-suffix>": [{"status": 429, "body": {"retry_after": 0.05}}, {"status": 200, "body": {}}]}}`
      — canned per-path Discord response sequences; a request matches on
      equal method + path suffix, each request consumes the next entry, and
      the last entry repeats when exhausted. Unmatched Discord-API requests
      answer `200 {}`.
- `GET /__logs` — recorded invocations:
  `[{function_name, invocation_type, payload}]`
- `GET /__discord_requests` — every Discord-API-shaped request (any method on
  `/api/v<N>/...`), in order: `[{method, path, body}]`
- `GET /__discord_patches` — legacy log of `PATCH .../messages/@original`
  requests: `[{application_id, interaction_token, payload}]` (superseded by
  `__discord_requests`)
- `POST /2015-03-31/functions/<name>/invocations` — Lambda-style invocation
  endpoint
- `POST /` with header `X-Amz-Target: DynamoDB_20120810.<Op>` — minimal
  DynamoDB surface: `PutItem` (honors the
  `attribute_not_exists(interaction_id)` `ConditionExpression` → `400` with
  the real `ConditionalCheckFailedException` `__type` shape when the id
  exists; unconditional puts overwrite) and `GetItem` (`{"Item": ...}` or
  `{}`) against a single in-memory table keyed by the `interaction_id` `S`
  value. `__reset` clears the table.
- `GET /__dynamodb_table` — debug view of the current mock DynamoDB items,
  keyed by interaction id

## Static checks and the pre-commit hook

Install the versioned pre-commit hook:

```bash
scripts/install-git-hooks.sh
```

The hook runs `scripts/pre-commit-static-checks.sh` for staged C++ Lambda
targets. The script selects affected Lambda folders from the staged index by
default; pass `--diff-range <ref>...<ref>` (or set
`STATIC_CHECKS_DIFF_RANGE`) to select from `git diff --name-only <range>`
instead — this is how CI analyzes a whole PR on a fresh checkout with an
empty index.

## Continuous Integration

`.github/workflows/ci.yml` runs on every push to `main` and every pull
request, with one in-flight run per ref (a new push cancels the previous
run). Three jobs:

- **cpp-unit** — builds the builder image for x86_64 (with a GitHub Actions
  layer cache so the aws-lambda-cpp / aws-sdk-cpp compile is reused), then
  runs `scripts/test-unit.sh`.
- **python-unit** — `python3 -m pytest tests/unit/python` on plain Python
  3.11, no Docker.
- **static-checks** — computes the PR/push diff range and runs
  `scripts/pre-commit-static-checks.sh --diff-range <range>`. For any
  affected Discord Lambda this builds the gateway zips and runs the RIE
  integration harness under valgrind, forced to native `linux/amd64`
  (`DISCORD_TEST_PLATFORM`) so no QEMU arm64 emulation is involved.

`scripts/test-local-discord-lambdas.sh` is not invoked directly in CI because
its default `linux/arm64` RIE run needs slow, flaky QEMU emulation on the
x86_64 GitHub-hosted runners.
