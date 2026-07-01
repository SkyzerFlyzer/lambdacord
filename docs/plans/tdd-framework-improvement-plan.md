# Lambdacord TDD Framework Improvement Plan & Specification

**Status:** Ready for implementation
**Intended executor:** An Opus-class orchestrating model dispatching one dedicated subagent per task
**Methodology:** Strict test-driven development (red → green → refactor) for every task
**Scope:** Framework code only (`src/`, `scripts/`, `tests/`, `docker/`, `infra/terraform/` root files). No `modules/` changes.

---

## 1. Purpose

Bring Lambdacord's generic Discord interactions framework to feature parity with mature
open-source Discord frameworks (discord.js, discord.py / app_commands, interactions.py,
serenity/poise, disgo) **within its own architectural model**: stateless C++ Lambdas behind
an HTTPS interactions endpoint, no gateway websocket, async worker fan-out.

Parity here means the *interaction-endpoint* feature set of those frameworks:

| Capability | discord.js / discord.py equivalent | Lambdacord today |
|---|---|---|
| Typed option access + resolved data | `interaction.options.getString(...)`, `resolved` | ❌ manual JSON digging |
| Embed builders with limit enforcement | `EmbedBuilder` | ❌ raw JSON, one `safe_truncate` convention |
| Component builders (buttons, all select types) | `ActionRowBuilder`, `StringSelectMenuBuilder`… | ⚠️ link button + 2-button paginator only |
| Modal builders | `ModalBuilder`, `TextInputBuilder` | ❌ |
| Followup / edit / delete webhook messages | `interaction.followUp()`, `deleteReply()` | ⚠️ PATCH `@original` only |
| Rate-limit-aware REST (429 retry, 5xx retry) | built-in REST managers | ❌ single attempt, throw |
| Context menu commands (user / message) | `ContextMenuCommandBuilder` | ❌ routing assumes CHAT_INPUT |
| Ephemeral/deferred response control per command | `deferReply({ephemeral: true})` | ❌ gateway hardcodes non-ephemeral defer |
| Autocomplete helpers (choices, 25-cap, filtering) | `AutocompleteInteraction.respond()` | ⚠️ raw passthrough |
| Permissions / contexts / integration_types | schema fields + checks | ❌ not validated, not exposed |
| Localization in command schemas | `name_localizations` etc. | ❌ not validated |
| Snowflake + mention/timestamp formatting utils | `SnowflakeUtil`, `time()`, `userMention()` | ❌ |
| Custom-id state encoding | various (customId parsing conventions) | ⚠️ prefix + single page number |
| Fast unit tests + CI | jest/pytest suites, GitHub Actions | ❌ Docker integration harness only |
| Scaffolding CLI | `create-discord-bot`, cookiecutters | ❌ manual folder creation |

The single biggest blocker to TDD in this repo is that **there is no fast test layer**:
the only tests build all zips and boot RIE containers. Phase 0 fixes that first; every
later task depends on it.

---

## 2. Current state (verified against source)

- `src/lambdas/discord-interactions/main.cpp` — ingress: Ed25519 verify (libsodium),
  dispatch by type. Type 2/5 → async + `{type:5}` defer; type 3 → async + `{type:6}`;
  type 4 → sync invoke; type 1 → inline pong. Hardcoded downstream function names.
- `src/lambdas/discord-application-command-handler|message-component-handler|modal-handler|autocomplete-handler/main.cpp`
  — routers deriving `discord-cmd-<path>` / `discord-component-<prefix>` /
  `discord-modal-<prefix>` / `discord-autocomplete-<path>-<option>` names.
- `src/include/discord_interactions/*.hpp` — header-only helpers:
  `interaction.hpp` (metadata, command path, component prefix), `response.hpp`
  (response type constants, `patch_original_response` via curl, `ephemeral_message`,
  `link_button_row`), `paginator.hpp` (Prev/Next row), `errors.hpp` (`ModuleError`,
  categories, `MODULE_ERROR` macro), `lambda_client.hpp` (sync/async invoke),
  `routing.hpp` (`ModuleRoute` from manifest).
- `scripts/lib/discord_modules.py` — module manifest discovery/merging/validation.
- `tests/local/discord/run_local_tests.py` — Docker RIE integration harness with mock
  Lambda control plane (`mock_lambda_server.py`) and mock Discord API surface.
- Build: `scripts/build-lambda.sh` in an AL2023 Docker builder image; auto-generated
  CMakeLists unless one exists in the Lambda folder.
- No C++ unit tests, no Python unit tests, no CI workflow.

Constraints that every task must respect (from `CLAUDE.md` / `AGENTS.md`):

- C++17, value-initialize everything with `{}`, single `main.cpp` per Lambda, shared code
  goes in `src/include/discord_interactions/` headers.
- Async workers PATCH `@original`; Lambda return values are never user-visible.
- Never leak internal error text (`ex.what()`, upstream bodies, SDK errors) to Discord
  users; log to stderr, send friendly copy.
- Ephemeral responses use `"flags": 64`.
- Generated Terraform (`infra/terraform/generated_*.tf`) is never hand-edited.
- Naming convention `discord-cmd-<group>-<subcommand>` is load-bearing.

---

## 3. TDD methodology specification

Every task in §5 follows the same cycle. Subagents must execute it in order and must not
skip the failing-test step.

1. **RED** — Write the tests described in the task's *Test specification* first. Run the
   relevant suite and confirm the new tests **fail** (or fail to compile because the API
   does not exist yet). A task that starts by writing implementation code is
   non-compliant.
2. **GREEN** — Write the minimal implementation that makes the new tests pass. Run the
   full suite of that layer (not just the new tests).
3. **REFACTOR** — Remove duplication introduced, keep public API as specified, re-run
   the suite.
4. **REGRESS** — Run every fast layer that exists at that point:
   - `scripts/test-unit.sh` (C++ unit tests, after task T0.1 exists)
   - `python3 -m pytest tests/unit/python` (after task T0.2 exists)
   - `scripts/pre-commit-static-checks.sh`
   - The Docker integration harness (`scripts/test-local-discord-lambdas.sh`) is
     required only for tasks that change Lambda `main.cpp` files or the harness itself;
     it needs Docker and full zip builds, so header-only tasks may skip it and say so.
5. **COMMIT** — One commit per task, message format:
   `feat(<area>): <summary> [T<id>]` or `test(<area>): ...` / `chore(<area>): ...`.
   The commit must contain both tests and implementation.

### Test layers

| Layer | Location | Runner | Speed | When required |
|---|---|---|---|---|
| C++ unit (doctest) | `tests/unit/cpp/` | `scripts/test-unit.sh` (Docker builder image, host-arch compile, no zip) | seconds | every header/library task |
| Python unit (pytest) | `tests/unit/python/` | `python3 -m pytest tests/unit/python` | seconds | every `scripts/` task |
| Integration (RIE) | `tests/local/discord/` | `scripts/test-local-discord-lambdas.sh` | minutes | Lambda `main.cpp` / routing / harness tasks |

### Definition of done (applies to every task)

- New tests written first and initially failing; final state all green.
- No changes outside the files listed in the task (plus test files) without a stated
  reason in the commit body.
- Public API matches the task's *API specification* exactly (names, signatures,
  namespaces) so downstream tasks can depend on it without renegotiation.
- Style rules from §2 constraints honored (value-init `{}`, no leaked internals, C++17).
- `CLAUDE.md` / `AGENTS.md` / `README.md` updated when a task adds a user-facing script
  or a new env variable (the task says so explicitly when required).

---

## 4. Architecture decisions fixed up front

Subagents must treat these as settled; do not re-litigate them mid-task.

- **AD-1: Test framework is doctest** (single header, fetched via CMake `FetchContent`,
  same mechanism as nlohmann/json). Unit tests compile inside the existing builder image
  for the host architecture — no zip packaging, no RIE.
- **AD-2: All new C++ code is header-only** under `src/include/discord_interactions/`,
  in namespace `discord_interactions`, so unit tests and Lambdas consume it identically.
  Exception: nothing. If something seems to need a .cpp, split the task instead.
- **AD-3: New REST features extend `response.hpp`'s curl approach** into a dedicated
  `rest.hpp` with an injectable base URL (existing `DISCORD_API_BASE_URL` override) so
  the integration harness's mock Discord server keeps working.
- **AD-4: Routing changes stay mechanical.** New route kinds get the same
  "derive-name-from-payload" treatment: user commands → `discord-usercmd-<name>`,
  message commands → `discord-msgcmd-<name>` (names lowercased, spaces → `-`).
- **AD-5: Per-route response behavior is declared in module manifests and delivered
  to the ingress as generated configuration.** Module authors set
  `"ephemeral_defer": true` on a command route in `module.manifest.json`;
  `scripts/generate-terraform-modules.py` merges these across installed modules and
  emits the resulting comma-separated command-path list into the ingress Lambda's
  `DISCORD_EPHEMERAL_DEFER_ROUTES` env var (generated Terraform). The ingress stays
  stateless and mechanical — it only parses the env var; it never reads manifests at
  runtime. Declaration lives with the command that owns it, so there is no
  config-drift failure mode. (T3.2 specifies details.)
- **AD-6: Python tooling changes must go through `scripts/lib/discord_modules.py`**, not
  duplicated parsing in entry-point scripts.
- **AD-7: No new runtime dependencies** beyond what the builder image already has
  (libsodium, libcurl, aws-lambda-cpp, aws-sdk-cpp Lambda client, nlohmann/json).
  doctest is test-only.

---

## 5. Task breakdown

Tasks are sized for **one subagent pass each**: touch ≤ ~6 files, one coherent API,
fully testable at one layer. Dependencies are explicit; anything not listed as a
dependency can run in parallel.

Task ID format: `T<phase>.<n>`.

---

### Phase 0 — Test infrastructure (enabler; serialize T0.1 → T0.2 → T0.3, then parallel)

#### T0.1 — C++ unit test runner (doctest + Docker script)

- **Goal:** A fast C++ unit test layer that compiles `tests/unit/cpp/*.cpp` against
  `src/include/` inside the existing builder image and runs them on the host arch.
- **Files:** `tests/unit/cpp/CMakeLists.txt`, `tests/unit/cpp/test_smoke.cpp`,
  `scripts/test-unit.sh`, `scripts/test-unit-in-docker.sh` (internal), README + CLAUDE.md
  script-ownership table updates.
- **API specification:**
  - `scripts/test-unit.sh [--filter <doctest-filter>]` — builds (respecting
    `LAMBDA_SKIP_IMAGE_BUILD=1`) and runs all unit tests; exit code propagates.
  - CMake target `discord_unit_tests`; doctest and nlohmann/json via `FetchContent`;
    `include_directories(src/include)`. No AWS SDK, no libsodium, no curl linkage —
    tests that need those belong to headers that must keep such includes optional
    (see per-task notes; where unavoidable, link the lib in the unit CMake).
- **Test specification (RED):** `test_smoke.cpp` asserts
  `discord_interactions::route_suffix_from_path("a b") == "a-b"` and one
  `header_value` case-insensitivity case. These pass immediately once the runner works —
  for this bootstrap task only, RED = the runner script failing before files exist.
- **Acceptance:** `scripts/test-unit.sh` prints a doctest summary and exits 0; a
  deliberately broken assertion exits non-zero. Docs updated.
- **Depends on:** nothing.

#### T0.2 — Python unit test layer for `scripts/lib`

- **Goal:** pytest coverage for the existing manifest/validation logic so later tooling
  tasks can TDD against it.
- **Files:** `tests/unit/python/test_discord_modules.py`, `tests/unit/python/conftest.py`
  (tmp-dir manifest fixtures), optional `pytest.ini` at repo root; CLAUDE.md note.
- **Test specification (RED first where behavior is new):** characterization tests for
  current behavior of `scripts/lib/discord_modules.py`: manifest discovery, route
  merging, duplicate-route rejection, schema merging, validation failure messages.
  Minimum 12 test cases including at least 3 invalid-manifest cases.
- **Acceptance:** `python3 -m pytest tests/unit/python -q` green; no changes to
  `discord_modules.py` behavior (pure characterization) unless a genuine bug is found —
  if found, fix it in the same task with a failing test first and call it out in the
  commit body.
- **Depends on:** nothing.

#### T0.3 — CI workflow

- **Goal:** GitHub Actions running both fast layers + static checks on every push/PR.
- **Files:** `.github/workflows/ci.yml`.
- **Specification:** jobs: (1) `cpp-unit` — build builder image (cached via
  `docker/build-push-action` layer cache or plain buildx cache), run
  `scripts/test-unit.sh`; (2) `python-unit` — pytest; (3) `static-checks` — run
  `scripts/pre-commit-static-checks.sh`. Integration harness is **not** run in CI
  (needs arm64 RIE emulation; document as a follow-up comment in the workflow).
- **Test specification:** CI config is validated by the workflow run itself; locally,
  run each job's command verbatim and paste output in the commit body.
- **Acceptance:** all three commands succeed locally exactly as written in the yml.
- **Depends on:** T0.1, T0.2.

---

### Phase 1 — Core library parity (all tasks depend on T0.1 only; parallelizable)

#### T1.1 — Typed command option access (`options.hpp`)

- **Goal:** discord.py-style option extraction from interaction JSON.
- **Files:** `src/include/discord_interactions/options.hpp`,
  `tests/unit/cpp/test_options.cpp`.
- **API specification (namespace `discord_interactions`):**
  ```cpp
  // Walks nested subcommand/group options to the innermost option list.
  json command_options(const json& interaction);
  std::optional<std::string> option_string(const json& interaction, const std::string& name);
  std::optional<int64_t>     option_int(const json& interaction, const std::string& name);
  std::optional<double>      option_number(const json& interaction, const std::string& name);
  std::optional<bool>        option_bool(const json& interaction, const std::string& name);
  // Snowflake-valued options (user/channel/role/mentionable/attachment id):
  std::optional<std::string> option_id(const json& interaction, const std::string& name);
  // Resolved-data lookups (empty json object if absent):
  json resolved_user(const json& interaction, const std::string& user_id);
  json resolved_member(const json& interaction, const std::string& user_id);
  json resolved_channel(const json& interaction, const std::string& channel_id);
  json resolved_role(const json& interaction, const std::string& role_id);
  json resolved_attachment(const json& interaction, const std::string& attachment_id);
  ```
  Type-mismatch (asking `option_int` for a string option) returns `std::nullopt`, never
  throws. Missing `data` returns nullopt/empty.
- **Test specification:** ≥ 15 cases: top-level options; one-level subcommand; group +
  subcommand nesting; each typed getter happy path; type mismatch → nullopt; absent
  option → nullopt; resolved user/member/channel/role/attachment lookups; interaction
  with no `data`. Use real-shaped Discord interaction fixtures (inline JSON in test).
- **Depends on:** T0.1.

#### T1.2 — Discord limits + safe truncation (`limits.hpp`)

- **Goal:** One authoritative header for Discord's documented limits and safe clamping.
- **Files:** `src/include/discord_interactions/limits.hpp`,
  `tests/unit/cpp/test_limits.cpp`.
- **API specification:**
  ```cpp
  namespace limits {
    inline constexpr size_t content = 2000, embed_title = 256, embed_description = 4096,
      embed_fields = 25, embed_field_name = 256, embed_field_value = 1024,
      embed_footer = 2048, embed_author = 256, embed_total = 6000, embeds_per_message = 10,
      action_rows = 5, buttons_per_row = 5, select_options = 25,
      autocomplete_choices = 25, choice_name = 100, custom_id = 100,
      modal_title = 45, text_input_label = 45, text_input_value = 4000;
  }
  // UTF-8-safe truncation: never splits a multibyte sequence; appends "…" when cut.
  std::string safe_truncate(const std::string& text, size_t max_bytes);
  ```
- **Test specification:** truncation shorter-than-limit passthrough; exact-limit
  passthrough; ASCII cut; multibyte (e.g. 4-byte emoji) boundary never split — result is
  valid UTF-8 and ≤ max_bytes; ellipsis behavior; zero-length edge. ≥ 10 cases.
- **Depends on:** T0.1.

#### T1.3 — Embed builder (`embed.hpp`)

- **Goal:** Fluent, limit-enforcing embed construction.
- **Files:** `src/include/discord_interactions/embed.hpp`,
  `tests/unit/cpp/test_embed.cpp`.
- **API specification:**
  ```cpp
  class EmbedBuilder {
  public:
    EmbedBuilder& title(const std::string&);        // safe_truncate to limits
    EmbedBuilder& description(const std::string&);
    EmbedBuilder& url(const std::string&);
    EmbedBuilder& color(uint32_t rgb);
    EmbedBuilder& timestamp_iso8601(const std::string&);
    EmbedBuilder& footer(const std::string& text, const std::string& icon_url = "");
    EmbedBuilder& author(const std::string& name, const std::string& url = "",
                         const std::string& icon_url = "");
    EmbedBuilder& thumbnail(const std::string& url);
    EmbedBuilder& image(const std::string& url);
    EmbedBuilder& field(const std::string& name, const std::string& value,
                        bool inline_field = false);   // silently drops past 25 fields
    json build() const;   // throws ModuleError(code="embed_too_large",
                          // category=validation) if total chars > 6000
  };
  ```
  All string setters clamp through `safe_truncate` with the matching limit.
- **Test specification:** ≥ 12 cases: each setter reflected in `build()` JSON; field
  cap at 25; per-field truncation; 6000-total overflow throws `ModuleError` with the
  specified code/category; empty builder produces `{}`-equivalent minimal object.
- **Depends on:** T0.1, T1.2 (limits), uses `errors.hpp`.

#### T1.4 — Component builders (`components.hpp`)

- **Goal:** Full component v1 coverage: buttons (all styles), string select, user/role/
  channel/mentionable selects, action-row composition with row limits.
- **Files:** `src/include/discord_interactions/components.hpp`,
  `tests/unit/cpp/test_components.cpp`. Refactor `link_button_row` in `response.hpp`
  to delegate here (keep the old function as a thin wrapper — do not break callers).
- **API specification:**
  ```cpp
  enum class ButtonStyle { primary = 1, secondary = 2, success = 3, danger = 4, link = 5 };
  json button(ButtonStyle, const std::string& custom_id_or_url, const std::string& label,
              bool disabled = false, const json& emoji = json());
  json string_select(const std::string& custom_id, const json& options /*array*/,
                     const std::string& placeholder = "", int min_values = 1,
                     int max_values = 1, bool disabled = false);
  json select_option(const std::string& label, const std::string& value,
                     const std::string& description = "", bool is_default = false);
  json user_select(const std::string& custom_id, const std::string& placeholder = "");
  json role_select(const std::string& custom_id, const std::string& placeholder = "");
  json channel_select(const std::string& custom_id, const std::string& placeholder = "",
                      const json& channel_types = json::array());
  json action_row(const json& components /*array*/);   // throws ModuleError
                    // (code="too_many_components", validation) if >5 buttons or >1 select
  json rows(std::initializer_list<json> row_list);      // throws if >5 rows
  ```
  `button` with `ButtonStyle::link` emits `url`, otherwise `custom_id`; custom_id
  clamped to 100 chars via `limits`.
- **Test specification:** ≥ 15 cases covering each factory's JSON shape (assert exact
  `type` codes: row 1, button 2, string select 3, user 5, role 6, mentionable 7,
  channel 8), link-vs-custom_id branch, row overflow throws, 5-row cap, custom_id clamp,
  `link_button_row` backwards-compat shape unchanged.
- **Depends on:** T0.1, T1.2.

#### T1.5 — Modal + text input builders (`modal.hpp`)

- **Goal:** Build modal interaction responses and parse modal-submit values.
- **Files:** `src/include/discord_interactions/modal.hpp`,
  `tests/unit/cpp/test_modal.cpp`.
- **API specification:**
  ```cpp
  enum class TextInputStyle { short_input = 1, paragraph = 2 };
  json text_input(const std::string& custom_id, const std::string& label,
                  TextInputStyle style, bool required = true,
                  const std::string& placeholder = "", const std::string& value = "",
                  int min_length = 0, int max_length = 0 /*0 = omit*/);
  json modal(const std::string& custom_id, const std::string& title,
             std::initializer_list<json> text_inputs);  // wraps each input in a row;
                                                        // throws ModuleError if >5
  // Extraction from a MODAL_SUBMIT interaction:
  std::optional<std::string> modal_value(const json& interaction, const std::string& custom_id);
  ```
  `modal(...)` returns the full `{type:9, data:{...}}` interaction response.
- **Test specification:** ≥ 10 cases: input JSON shape per style; optional fields
  omitted when defaulted; title/label clamped via limits; >5 inputs throws;
  `modal_value` finds nested value, returns nullopt when absent; real-shaped
  MODAL_SUBMIT fixture.
- **Depends on:** T0.1, T1.2.

#### T1.6 — Snowflakes, mentions, timestamp formatting (`format.hpp`)

- **Goal:** The utility belt every framework ships.
- **Files:** `src/include/discord_interactions/format.hpp`,
  `tests/unit/cpp/test_format.cpp`.
- **API specification:**
  ```cpp
  bool is_snowflake(const std::string&);                 // 17–20 digit numeric
  int64_t snowflake_timestamp_ms(const std::string&);    // Discord epoch 1420070400000;
                                                         // throws ModuleError(validation) on bad input
  std::string user_mention(const std::string& id);       // <@id>
  std::string channel_mention(const std::string& id);    // <#id>
  std::string role_mention(const std::string& id);       // <@&id>
  enum class TimestampStyle { short_time, long_time, short_date, long_date,
                              short_datetime, long_datetime, relative };
  std::string discord_timestamp(int64_t unix_seconds, TimestampStyle);
  std::string escape_markdown(const std::string&);       // \ before *_~`|>#-
  std::string code_block(const std::string& body, const std::string& lang = "");
  ```
- **Test specification:** ≥ 14 cases: snowflake validity edges (16/17/20/21 digits,
  non-numeric); known snowflake → known timestamp; each mention format; each timestamp
  style letter (`t T d D f F R`); markdown escaping including already-escaped input;
  code block with/without lang and with embedded backticks (use zero-width-space or
  documented strategy — specify in test first).
- **Depends on:** T0.1. (Uses `errors.hpp`.)

#### T1.7 — Structured custom_id state codec (`custom_id.hpp`)

- **Goal:** Replace ad-hoc `prefix:arg` handling with a validated codec usable by
  paginator and module components.
- **Files:** `src/include/discord_interactions/custom_id.hpp`,
  `tests/unit/cpp/test_custom_id.cpp`; refactor `paginator.hpp` to use it (public
  paginator API unchanged).
- **API specification:**
  ```cpp
  // encode("page", {"3", "user42"}) -> "page:3:user42"; validates total ≤ 100 chars
  // (throws ModuleError validation), args must not contain ':'.
  std::string encode_custom_id(const std::string& prefix, const std::vector<std::string>& args);
  struct CustomId { std::string prefix{}; std::vector<std::string> args{}; };
  CustomId parse_custom_id(const std::string& raw);  // never throws on well-formed
                                                     // "prefix" alone; throws ModuleError on empty prefix
  ```
- **Test specification:** ≥ 10 cases: round-trip encode/parse; prefix-only; empty args
  elements preserved; `:` in arg throws; 100-char limit throws; parse of legacy
  paginator ids (`p:3`); paginator regression tests still green unchanged.
- **Depends on:** T0.1, T1.2.

#### T1.8 — Autocomplete response helpers (`autocomplete.hpp`)

- **Goal:** Safe choice construction like `AutocompleteInteraction.respond()`.
- **Files:** `src/include/discord_interactions/autocomplete.hpp`,
  `tests/unit/cpp/test_autocomplete.cpp`.
- **API specification:**
  ```cpp
  json choice(const std::string& name, const std::string& value);
  json choice(const std::string& name, int64_t value);
  json choice(const std::string& name, double value);
  // Caps at 25, truncates names to 100 via limits, returns {type:8, data:{choices:[...]}}
  json autocomplete_response(const json& choices /*array*/);
  // Case-insensitive prefix-then-substring filter helper over {name,value} array:
  json filter_choices(const json& choices, const std::string& query);
  // Which option is focused (name), from the interaction:
  std::optional<std::string> focused_option_name(const json& interaction);
  std::optional<std::string> focused_option_value(const json& interaction);
  ```
- **Test specification:** ≥ 10 cases: each choice overload's JSON; >25 choices capped
  at 25 (first 25 kept); name truncation; filter ordering (prefix matches before
  substring matches, stable within class); focused option extraction from nested
  subcommand fixture; no focused option → nullopt.
- **Depends on:** T0.1, T1.2.

---

### Phase 2 — REST capabilities (serialize T2.1 → T2.2; T2.3 after T2.1)

#### T2.1 — Rate-limit-aware webhook REST core (`rest.hpp`)

- **Goal:** Extract the curl plumbing from `response.hpp` into a reusable request core
  with 429/5xx retry — the equivalent of discord.js's REST manager for the
  webhook-token surface this framework uses.
- **Files:** `src/include/discord_interactions/rest.hpp`,
  `tests/unit/cpp/test_rest.cpp`; `response.hpp`'s `patch_original_response` becomes a
  thin wrapper (signature unchanged).
- **API specification:**
  ```cpp
  struct RestResponse { long status = 0; std::string body{}; };
  struct RestRetryPolicy { int max_attempts = 3; long max_total_wait_ms = 8000; };
  // Executes with retries: on 429 sleeps min(retry_after from JSON body or
  // Retry-After header, remaining budget); on 5xx exponential backoff 250ms*2^n.
  // Throws ModuleError(code="discord_api_error", category=upstream) after exhaustion;
  // 4xx (non-429) throws immediately with status in safe_context.
  RestResponse discord_request(const std::string& method, const std::string& url,
                               const json& body = json(),
                               const RestRetryPolicy& policy = {});
  // Convenience (webhook-token surface):
  std::string webhook_url(const std::string& application_id, const std::string& token,
                          const std::string& suffix /* "/messages/@original" etc. */);
  ```
  Include a `User-Agent: DiscordBot (lambdacord, 1.0)` header on every request.
  **Testability requirement:** the retry/parse logic (`parse_retry_after(status,
  headers_json, body)` and `backoff_ms(attempt)`) must be exposed as pure functions so
  unit tests cover them without network; `discord_request` itself is covered by the
  integration harness (T2.3 wires mock-server assertions).
- **Test specification (unit):** ≥ 10 cases on the pure functions: retry_after from
  JSON body (float seconds → ms), from header, missing → default; backoff sequence;
  budget clamping; 4xx classification; URL joiner with/without leading slash.
- **Depends on:** T0.1; uses `errors.hpp`.

#### T2.2 — Followup / edit / delete interaction messages (`webhook_messages.hpp`)

- **Goal:** Full interaction-webhook message lifecycle: `followUp`, `editReply`,
  `deleteReply`, fetch original.
- **Files:** `src/include/discord_interactions/webhook_messages.hpp`,
  `tests/unit/cpp/test_webhook_messages.cpp`.
- **API specification:**
  ```cpp
  json  create_followup(const std::string& application_id, const std::string& token,
                        const json& message);              // POST /webhooks/APP/TOKEN
  json  get_original(const std::string& application_id, const std::string& token);
  void  edit_followup(const std::string& application_id, const std::string& token,
                      const std::string& message_id, const json& message);
  void  delete_original(const std::string& application_id, const std::string& token);
  void  delete_followup(const std::string& application_id, const std::string& token,
                        const std::string& message_id);
  ```
  All built on `discord_request`. Unit tests cover URL construction (expose
  `followup_path(...)` helpers as pure functions); behavior is integration-tested in
  T2.3.
- **Test specification (unit):** ≥ 8 URL/path construction and payload passthrough
  cases (method + path per operation).
- **Depends on:** T2.1.

#### T2.3 — Integration coverage for REST layer (mock Discord API growth)

- **Goal:** Extend `tests/local/discord/mock_lambda_server.py`'s Discord-API surface to
  record method+path for webhook message routes and serve canned 429-then-200 sequences;
  add an integration suite exercising a test fixture Lambda (a minimal
  `tests/local/discord/fixtures/discord-cmd-test-echo/main.cpp` built only for tests)
  that does defer-PATCH, followup, delete.
- **Files:** `tests/local/discord/mock_lambda_server.py`,
  `tests/local/discord/run_local_tests.py` (new suite `rest`),
  `tests/local/discord/fixtures/discord-cmd-test-echo/main.cpp`, harness docs in
  CLAUDE.md Local Testing section.
- **Test specification:** mock returns one 429 with `retry_after: 0.05` then 200 —
  suite asserts two attempts recorded and final success; followup POST recorded with
  correct path; delete recorded. Python-unit-test the new mock endpoints too
  (`tests/unit/python/test_mock_lambda_server.py`) since the mock is plain Python.
- **Acceptance:** `scripts/test-local-discord-lambdas.sh` green including new suite;
  suite selectable via existing suite-selection mechanism.
- **Depends on:** T2.1, T2.2, T0.2.

---

### Phase 3 — Gateway & routing features

#### T3.1 — Context menu commands (user + message commands)

- **Goal:** Route command type 2 (USER) and 3 (MESSAGE) like other frameworks do.
- **Files:** `src/lambdas/discord-application-command-handler/main.cpp`,
  `src/include/discord_interactions/interaction.hpp` (add
  `application_command_kind(interaction)` returning enum {chat_input, user, message}
  and `context_menu_route_suffix(name)` — lowercase, spaces→`-`),
  unit tests `tests/unit/cpp/test_interaction_routing.cpp`, integration suite additions
  in `run_local_tests.py`, docs (CLAUDE.md routing diagram + naming convention).
- **Specification:** `data.type == 2` → invoke `discord-usercmd-<suffix>`;
  `data.type == 3` → `discord-msgcmd-<suffix>`; absent/1 → existing behavior exactly.
  Python side: `discord_modules.py` route-kind vocabulary gains `user_commands` and
  `message_commands`; registration validation accepts type-2/3 schema entries (no
  `description`, no `options` — reject if present, matching Discord rules).
- **Test specification:** C++ unit: kind detection fixtures (types 1/2/3/missing),
  suffix derivation ("Report User" → `report-user`). Python unit: schema validation
  accepts/rejects correctly. Integration: type-2 command interaction routes to
  `discord-usercmd-report-user` (async, recorded by mock).
- **Depends on:** T0.1, T0.2; integration parts after T0.3 optional.

#### T3.2 — Manifest-driven ephemeral defer at ingress

- **Goal:** Let commands opt into an ephemeral deferred ACK (`{type:5, data:{flags:64}}`)
  — parity with `deferReply({ ephemeral: true })` — declared where the command lives
  (its module manifest), with the plumbing generated, not hand-maintained.
- **Files:** `src/lambdas/discord-interactions/main.cpp`,
  `src/include/discord_interactions/interaction.hpp` (pure helper:
  `bool route_in_csv_allowlist(const std::string& command_path, const std::string& csv)`),
  `scripts/lib/discord_modules.py` (manifest field parsing/validation + merged-list
  helper `ephemeral_defer_routes(manifests)`), `scripts/generate-terraform-modules.py`
  (emit merged list into the ingress env var in generated Terraform),
  C++ unit tests, `tests/unit/python/test_ephemeral_defer.py`, integration tests,
  env-var + manifest-field docs in CLAUDE.md + README.
- **Specification:**
  - **Manifest:** a command route entry may carry `"ephemeral_defer": true`. Today
    command routes map route → function-name string, so the schema gains an
    alternative object form: `"account link": {"lambda": "discord-cmd-account-link",
    "ephemeral_defer": true}`. The plain-string form remains valid and means
    `ephemeral_defer: false`; `routing.hpp`'s `route_from_manifest_entry` and all
    Python route parsing must accept both forms. Validation rejects
    `ephemeral_defer` on non-command route kinds and non-boolean values.
  - **Generator:** `scripts/generate-terraform-modules.py` collects all command paths
    with `ephemeral_defer: true` across installed modules (sorted, deduplicated) and
    emits them as a comma-separated string into the ingress Lambda's
    `DISCORD_EPHEMERAL_DEFER_ROUTES` env var in the generated Terraform. Empty list →
    variable omitted or empty; both must be handled by the ingress.
  - **Ingress:** reads optional env `DISCORD_EPHEMERAL_DEFER_ROUTES` (comma-separated
    command paths, e.g. `account link,account unlink`). On type 2, if the command path
    matches → respond `{type:5, data:{flags:64}}`; else current `{type:5}`. Matching
    is exact on the full path, whitespace-trimmed per entry. Component (type 3) and
    modal (type 5) behavior unchanged. The ingress never reads manifests at runtime.
- **Test specification:**
  - C++ unit: allowlist parser (empty/unset env, one entry, trim, no-match,
    exact-match-only — `account` must not match `account link`);
    `route_from_manifest_entry` accepts both string and object route forms.
  - Python unit (write first): object-form route parsing; merged list is sorted +
    deduped across multiple module fixtures; string-form routes contribute nothing;
    validation errors for `ephemeral_defer` on component routes and for non-boolean
    values; generator output contains the env var with the expected value (assert on
    generated Terraform text against a tmp-dir module fixture); empty case emits no
    stale value.
  - Integration: with env set on the RIE container, ingress response body contains
    `flags: 64` for a listed command and not for an unlisted one; without env,
    unchanged.
- **Depends on:** T0.1, T0.2.

#### T3.3 — Async retry dedup guard (interaction idempotency)

- **Goal:** AWS async invokes retry on failure; Discord tokens are one-shot. Give worker
  Lambdas a standard guard so a retried invoke doesn't double-PATCH or double-act —
  the serverless analogue of frameworks' single-dispatch guarantee.
- **Files:** `src/include/discord_interactions/idempotency.hpp`,
  `tests/unit/cpp/test_idempotency.cpp`, CLAUDE.md guidance section.
- **Specification (deliberately storage-free):** in-process LRU of interaction ids
  (warm container catches same-payload retries):
  ```cpp
  class SeenInteractions {           // fixed capacity, default 128
  public:
    explicit SeenInteractions(size_t capacity = 128);
    bool seen_before(const std::string& interaction_id);  // false first time, true after; LRU evict
  };
  ```
  Document explicitly in CLAUDE.md that cross-container dedup requires module-owned
  storage and is out of framework scope. Do **not** wire it into router Lambdas in this
  task (workers own the decision).
- **Test specification:** ≥ 8 cases: first-seen false; second-seen true; eviction at
  capacity; eviction order (LRU not FIFO — re-seeing refreshes); capacity 1 edge;
  empty id handled.
- **Depends on:** T0.1.

#### T3.4 — Friendly unknown-route replies

- **Goal:** Today an unregistered route makes the router throw and the user sees a
  Discord "application did not respond"/eternal "thinking…". Frameworks surface a clean
  error. Routers should PATCH `@original` with friendly copy when the target Lambda
  doesn't exist (Lambda `ResourceNotFoundException`), then still fail the invocation for
  observability.
- **Files:** `src/lambdas/discord-application-command-handler/main.cpp`,
  `src/lambdas/discord-message-component-handler/main.cpp`,
  `src/lambdas/discord-modal-handler/main.cpp`,
  `src/include/discord_interactions/lambda_client.hpp` (surface a
  `bool is_function_not_found(const Aws::Lambda::LambdaClient outcome error)` style
  helper — spec the exact signature against the SDK error type in-task),
  integration suite additions.
- **Specification:** On invoke failure classified as function-not-found: PATCH
  `@original` with ephemeral-style content "That command isn't available right now."
  (exact copy in one shared constant), log the real error to stderr, return failure.
  Other failures: unchanged behavior. Autocomplete router: return empty choices
  `{type:8, data:{choices:[]}}` instead of PATCH (sync path).
- **Test specification:** integration: configure mock server to 404 a function name;
  assert the mock's Discord-API log records the PATCH with the friendly copy and that
  no internal error text appears in the PATCH body. Unit: the copy constant contains no
  format placeholders; classification helper against a fabricated SDK error object if
  feasible, else document why unit coverage is integration-only.
- **Depends on:** T0.1, T2.1 (uses `rest.hpp` PATCH path), T0.2.

---

### Phase 4 — Tooling & DX (Python; depend on T0.2)

#### T4.1 — Discord-rule command schema validation

- **Goal:** Validate what Discord will actually reject, at `--validate-only` time —
  parity with builder-level validation in discord.js.
- **Files:** `scripts/lib/discord_modules.py`,
  `tests/unit/python/test_schema_validation.py`.
- **Specification (validation rules, each with a distinct error message):**
  chat-input names match `^[-_\p{L}\p{N}]{1,32}$` and are lowercase; descriptions 1–100
  chars (required for CHAT_INPUT, forbidden for type 2/3); option names valid + unique
  per level; required options precede optional ones; ≤ 25 options per level; choices
  ≤ 25 with name ≤ 100 / value length rules; subcommand nesting depth ≤ 2; total
  command count per module reported; `default_member_permissions` if present is a
  numeric string; `integration_types` ⊆ {0,1}; `contexts` ⊆ {0,1,2};
  `name_localizations`/`description_localizations` keys are valid Discord locales
  (maintain the locale list as a module-level constant) and values obey the same length
  rules.
- **Test specification:** ≥ 20 pytest cases — one accepting fixture and one rejecting
  fixture per rule group, asserting on the specific error message substring.
- **Acceptance:** `register-discord-commands.py --validate-only` surfaces the new
  errors; existing valid manifests still pass (run against a fixture copy of a
  currently-valid schema).
- **Depends on:** T0.2.

#### T4.2 — Route ↔ schema ↔ Lambda-folder consistency check

- **Goal:** Catch the "naming convention is load-bearing" failure class mechanically:
  every command path in a module's schema must have a manifest route and a Lambda
  folder whose name matches the derived `discord-cmd-<path>` (and vice versa: no
  orphan routes/folders).
- **Files:** `scripts/lib/discord_modules.py` (new `check_route_consistency(...)`),
  wire into validation used by `register-discord-commands.py` and
  `scripts/pre-commit-static-checks.sh`; `tests/unit/python/test_route_consistency.py`.
- **Test specification:** ≥ 10 cases: fully consistent module fixture; schema command
  missing route; route missing schema entry; route → nonexistent folder; folder not
  referenced by any route (warning, not error — flag choice tested); autocomplete
  route naming (`discord-autocomplete-<path>-<option>`) checked against options with
  `"autocomplete": true`; user/message command kinds (post-T3.1 vocabulary) validated
  if present, tolerated absent (so this task does not hard-depend on T3.1).
- **Depends on:** T0.2. (Soft-coordinates with T3.1 route kinds.)

#### T4.3 — Scaffolding generator (`scripts/new-lambda.py`)

- **Goal:** Framework CLI parity (`create-discord-bot`-style DX): generate a correctly
  named command/component/modal/autocomplete Lambda skeleton inside a module, with
  manifest route, schema stub, and a reminder checklist.
- **Files:** `scripts/new-lambda.py`, template strings inside the script (no separate
  template dir), `tests/unit/python/test_new_lambda.py`, README user-docs section,
  CLAUDE.md script-ownership table.
- **Specification:**
  `python3 scripts/new-lambda.py --module <name> --kind command --path "group sub"`
  (kinds: `command`, `component`, `modal`, `autocomplete` — autocomplete also takes
  `--option <name>`): creates `modules/<name>/lambdas/<kind-dir>/<derived-lambda-name>/main.cpp`
  from a template that already follows every CLAUDE.md rule (metadata extraction via
  `interaction.hpp`, deferred PATCH via helpers, ModuleError mapping, value-init style),
  inserts the route into `module.manifest.json` (sorted, idempotent — refuses if route
  exists), appends a schema stub to `discord.commands.json` for command kind, prints
  next steps (build command, register command). `--dry-run` prints the plan without
  writing.
- **Test specification:** ≥ 12 cases against a tmp-dir fake module: files created with
  derived names; manifest updated and still valid per `discord_modules.py` validation;
  refusal on existing route; dry-run writes nothing; each kind's naming
  (`discord-cmd-…`, `discord-component-…`, `discord-modal-…`,
  `discord-autocomplete-…-<option>`); generated main.cpp contains the PATCH-@original
  call and no `ex.what()` passthrough to user copy (assert on template content).
- **Depends on:** T0.2, T4.2 (reuses consistency validation to verify its own output).

#### T4.4 — Paginator upgrade to full parity component set

- **Goal:** Match the common framework paginator (first/prev/counter/next/last,
  disabled-state correctness) using the new builders.
- **Files:** `src/include/discord_interactions/paginator.hpp`,
  `tests/unit/cpp/test_paginator.cpp`.
- **API specification:** keep `paginator_row(prefix, page, total_pages)` returning the
  **new** 5-button row: `⏮` first, `◀` prev, disabled counter button `"n / N"`, `▶`
  next, `⏭` last; custom_ids via `encode_custom_id(prefix, {"<target-page>"})`; keep
  `parse_page_after_prefix` semantics (accepts both old `p:3` and new ids — same wire
  format, so free); all buttons disabled when `total_pages <= 1`.
- **Test specification:** ≥ 10 cases: middle page enables all; first page disables
  first/prev; last page disables next/last; single page disables everything; counter
  label text; custom_id targets (first→0, last→N-1); zero total_pages edge;
  round-trip with `parse_page_after_prefix`.
- **Depends on:** T1.4, T1.7.
- **Breaking-change note:** the row shape changes (2 → 5 buttons). Modules consume the
  same function signature, so no code break; visual change only. State this in the
  commit body.

#### T4.5 — Documentation sync & framework docs page

- **Goal:** A `docs/framework-reference.md` documenting every public header API added
  by this plan (one section per header, signatures + one example each), plus CLAUDE.md
  routing-diagram updates (usercmd/msgcmd lanes), env var table additions
  (`DISCORD_EPHEMERAL_DEFER_ROUTES`), and README feature list refresh.
- **Files:** `docs/framework-reference.md`, `CLAUDE.md`, `AGENTS.md`, `README.md`.
- **Test specification:** documentation task — the "test" is a checklist in the task
  prompt: every header in `src/include/discord_interactions/` has a section; every env
  var read via `std::getenv` in `src/` appears in the CLAUDE.md tables (grep-verify and
  paste the grep in the commit body).
- **Depends on:** all other tasks (run last).

---

## 6. Dependency graph / suggested waves

```
Wave 0 (serial):   T0.1 → T0.2 → T0.3
Wave 1 (parallel): T1.1  T1.2  T1.6  T3.3          (need only T0.1/T0.2)
Wave 2 (parallel): T1.3  T1.4  T1.5  T1.7  T1.8    (need T1.2)   |  T2.1  |  T4.1  T4.2
Wave 3 (parallel): T2.2  T3.1  T3.2  T4.3           (T2.2 needs T2.1)
Wave 4 (parallel): T2.3  T3.4  T4.4
Wave 5 (serial):   T4.5
```

Rules for the orchestrator:

- Never run two tasks that list the same file in **Files** concurrently
  (e.g. T1.3/T1.4/T1.5 all touch only their own files — safe; T3.1 and T3.4 both touch
  `discord-application-command-handler/main.cpp` — serialize; T3.1 and T3.2 both touch
  `scripts/lib/discord_modules.py` within Wave 3 — serialize).
- Each task lands as one commit on the working branch before its dependents start.
- If a subagent reports an API-spec conflict (spec in §5 is wrong against reality),
  it must stop and return the conflict rather than improvise a different public API.

---

## 7. Subagent prompt

### 7.1 Shared preamble (prepend to every task dispatch verbatim)

```text
You are implementing one task of the Lambdacord TDD improvement plan
(docs/plans/tdd-framework-improvement-plan.md). Read that file's sections 2, 3, 4
before writing anything. You are working on branch <BRANCH> in a repo whose root
CLAUDE.md rules are binding.

Non-negotiable process:
1. RED: write the tests specified below FIRST. Run the test layer and paste the failing
   output into your working notes. If the tests pass before you implement anything,
   your tests are wrong — rewrite them.
2. GREEN: implement the minimal code to pass. Match the task's API specification
   EXACTLY — names, signatures, namespace, error codes. If the spec conflicts with
   reality (compiler, SDK, Discord docs), STOP and report the conflict; do not invent
   a different public API.
3. REFACTOR, then run every fast layer that exists: scripts/test-unit.sh;
   python3 -m pytest tests/unit/python -q; scripts/pre-commit-static-checks.sh.
   Run scripts/test-local-discord-lambdas.sh only if your task touches Lambda main.cpp
   files or the test harness (it requires Docker + scripts/build-all-lambdas.sh first).
4. Commit exactly once: "<type>(<area>): <summary> [T<id>]" with tests + implementation
   together. Do not push; the orchestrator pushes.

Hard constraints (violations = task failure):
- C++17, header-only under src/include/discord_interactions/, namespace
  discord_interactions, value-initialize every local/struct/JSON with {} or explicit
  values.
- Never emit internal error text (ex.what(), SDK/API bodies) into any user-facing
  Discord payload. Log to stderr; map to friendly copy or ModuleError.
- Do not touch modules/, infra/terraform/generated_*.tf, or files outside your task's
  Files list (plus your test files). Do not add runtime dependencies.
- Async workers PATCH @original; never rely on Lambda return values for user output.

Report back: files changed, test counts (added/passing), the RED-phase failing output
snippet, any spec conflicts, any documentation you updated.
```

### 7.2 Per-task dispatch template

```text
<SHARED PREAMBLE>

TASK <id> — <title>

Goal: <Goal from §5>
Files you may create/modify: <Files list>
API specification (exact): <API block>
Test specification (write these first): <Test spec>
Acceptance criteria: <Acceptance / Definition of done deltas>
Dependencies already merged, available for use: <list of merged task IDs and the
headers/functions they provide>
```

The orchestrator fills the template mechanically from §5. Example, fully expanded, for
the first parallelizable library task:

```text
<SHARED PREAMBLE>

TASK T1.2 — Discord limits + safe truncation

Goal: One authoritative header for Discord's documented limits and UTF-8-safe clamping.
Files you may create/modify:
  - src/include/discord_interactions/limits.hpp (new)
  - tests/unit/cpp/test_limits.cpp (new)
API specification (exact): namespace discord_interactions::limits with the constexpr
size_t constants listed in the plan (content=2000, embed_title=256,
embed_description=4096, embed_fields=25, embed_field_name=256, embed_field_value=1024,
embed_footer=2048, embed_author=256, embed_total=6000, embeds_per_message=10,
action_rows=5, buttons_per_row=5, select_options=25, autocomplete_choices=25,
choice_name=100, custom_id=100, modal_title=45, text_input_label=45,
text_input_value=4000) and
  std::string discord_interactions::safe_truncate(const std::string& text, size_t max_bytes);
safe_truncate must never split a multibyte UTF-8 sequence and appends "…" (U+2026,
3 bytes, counted within max_bytes) when it cuts.
Test specification (write these first): ≥10 doctest cases — passthrough under limit,
passthrough at exact limit, ASCII cut with ellipsis, 4-byte emoji straddling the cut
boundary (result must be valid UTF-8 and ≤ max_bytes), cut where the ellipsis itself
doesn't fit (max_bytes < 3 → empty string), zero max_bytes, empty input, multi-emoji
string cut mid-sequence, limit exactly at a codepoint boundary, and constants
spot-checks (embed_total == 6000 etc.).
Acceptance criteria: scripts/test-unit.sh green; no other suites affected; single
commit "feat(limits): add discord limits header with utf8-safe truncation [T1.2]".
Dependencies already merged, available for use: T0.1 (scripts/test-unit.sh,
tests/unit/cpp/CMakeLists.txt — add your test file to it).
```

---

## 8. Orchestrator (Opus-class) execution notes

1. Work on the designated feature branch; one commit per task; push after each wave.
2. Dispatch order per §6 waves; within a wave, dispatch subagents in parallel with
   isolated worktrees if the platform supports it (tasks are file-disjoint by design,
   but isolation removes CMakeLists merge friction in `tests/unit/cpp/` — if not using
   worktrees, serialize the edits to `tests/unit/cpp/CMakeLists.txt` by having T0.1
   use a `file(GLOB ...)` over `test_*.cpp` so later tasks never edit it; **prefer the
   GLOB approach and state it in T0.1's dispatch**).
3. After each wave: run all fast suites at the repo root yourself; if red, dispatch a
   fix-forward subagent with the failing output rather than reverting, unless the
   public API itself was implemented off-spec (then revert the task's commit and
   re-dispatch with the conflict resolved).
4. Escalation protocol: any subagent-reported spec conflict updates THIS document first
   (one commit, `docs:` prefix), then re-dispatch. The plan is the source of truth.
5. Full-system verification gates: after Wave 2 and again after Wave 4, run
   `scripts/build-all-lambdas.sh && scripts/test-local-discord-lambdas.sh` and fix
   regressions before proceeding.
6. Out of scope, do not accept scope creep from subagents: gateway/websocket features,
   voice, sharding, message-content intents, storage-backed dedup, OAuth flows,
   Components V2 (`IS_COMPONENTS_V2` flag) — record as future work instead.

## 9. Future work (explicitly deferred)

- Components V2 layout (`flags: 1<<15`, container/section components).
- Attachment/file upload support in webhook messages (multipart curl).
- Entitlements / premium interactions (`type 10 PREMIUM_REQUIRED` replies).
- Storage-backed cross-container interaction dedup.
- Localized runtime replies (framework-level copy catalogue).
- CI-side integration harness via arm64 runners or x86_64 RIE builds.
