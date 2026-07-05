# Scripts Reference

## User scripts

These are the scripts normal project users should reach for:

| Script | Purpose |
|---|---|
| `scripts/build-lambda.sh <lambda-folder>` | Build one C++ Lambda zip from a framework or module Lambda folder. |
| `scripts/build-all-lambdas.sh` | Build every Lambda declared by the framework and installed module manifests. Run this before Terraform deploys. |
| `scripts/test-unit.sh` | Run the fast C++ unit test suite (doctest) in seconds. Compiles `tests/unit/cpp/` against `src/include` inside the builder image; no zip build or emulator needed. Optional `--filter <doctest-filter>`. |
| `scripts/test-local-discord-lambdas.sh` | Run the local Discord Lambda integration suites against built zip artifacts. |
| `scripts/generate-terraform-modules.py` | Regenerate root Terraform module wiring from installed module manifests. |
| `scripts/terraform-deploy.sh <action>` | Regenerate module Terraform wiring, optionally build Lambdas, then run Terraform `init`, `validate`, `plan`, `apply`, or `output`. |
| `scripts/new-lambda.py --module <name> --kind command\|component\|modal\|autocomplete --path "..."` | Scaffold a correctly named module Lambda skeleton, wire its manifest route, and append a schema stub for command kinds. Autocomplete also takes `--option`; `--dry-run` previews without writing. |
| `scripts/register-discord-commands.py` | Merge installed module command schemas and bulk overwrite Discord application commands. `--validate-only` checks manifests, routes, schemas, Lambda folders, and Terraform folders without calling Discord. |
| `scripts/remove-discord-commands.py` | Clear registered Discord commands for a guild or globally (`--global`). |
| `scripts/print-module-route-map.py <kind>` | Print the merged module route map for `commands`, `components`, `modals`, or `autocomplete`. |
| `scripts/install-git-hooks.sh` | Install the repository pre-commit hook locally. |

## Helper / internal scripts

Lower-level scripts used by the build system, tests, hooks, or agents:

| Script | Intended use |
|---|---|
| `scripts/build-lambda-in-docker.sh` | Internal build implementation called by `scripts/build-lambda.sh` inside the builder container. Do not call it directly. |
| `scripts/test-unit-in-docker.sh` | Internal build+run implementation for the C++ unit suite, called by `scripts/test-unit.sh` inside the builder container. Do not call it directly. |
| `scripts/lib/discord_modules.py` | Shared Python helper for module manifest discovery, route merging, schema merging, and validation. Import from user-facing scripts instead of duplicating manifest parsing. |
| `scripts/pre-commit-static-checks.sh` | Hook/agent static-analysis runner. Selects affected Lambda folders from the staged index by default; pass `--diff-range <ref>...<ref>` (or set `STATIC_CHECKS_DIFF_RANGE`) to select from a git diff range instead — this is how CI analyzes a whole PR on a fresh checkout. |
| `scripts/mock-lambda-runtime-api.py` | Local/debug helper for Lambda runtime experiments, not part of the normal deploy flow. |
| `tests/local/discord/run_local_tests.py` | Test harness behind `scripts/test-local-discord-lambdas.sh`; call it directly only when selecting suites during development (`--suite <name>`). |
| `tests/local/discord/mock_lambda_server.py` | Local Lambda control-plane mock used by the test harness. See [Testing](testing.md#mock-server-api) for its API. |
