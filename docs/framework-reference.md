# Discord Interactions Framework Reference

This is the API reference for the header-only C++ framework under
`src/include/discord_interactions/`. Every Lambda in this repo (and in installed
modules) `#include`s these headers directly — there is no shared library and no
linked framework object. The headers are compiled into each Lambda's single
`main.cpp`, so the "framework" is entirely source-level: pure functions and small
builder types in the `discord_interactions` namespace, plus a handful of
executing helpers that wrap curl or the AWS SDK.

## Architecture contract

Read this before using any header — the contracts below are load-bearing across
the whole surface.

- **Headers are compiled into each Lambda.** They are header-only and
  dependency-light. Most (`interaction`, `response`, `errors`, `limits`,
  `options`, `embed`, `components`, `components_v2`, `modal`, `autocomplete`,
  `custom_id`, `format`, `paginator`, `premium`, `routing`, `unknown_route`,
  `rest_policy`, `idempotency`, `idempotency_requests`) depend only on the C++
  standard library and `nlohmann/json`, so the unit-test image can compile them
  without curl or the AWS SDK. Only `rest`/`webhook_messages` pull in curl, and
  only `lambda_client`/`idempotency_store` pull in the AWS SDK.
- **custom_id is the only free state channel.** In this serverless model there is
  no in-memory session between interactions: each Discord interaction lands on a
  fresh (or unrelated warm) Lambda. A component's `custom_id` is the sole piece of
  state Discord round-trips back to us, and it is capped at 100 bytes
  (`limits::custom_id`) — a hard architectural boundary enforced at encode time by
  `custom_id.hpp`. State that does not fit belongs in module-owned durable storage,
  keyed by a compact id that does fit.
- **AD-8 — all in-Lambda waiting is bounded by the invocation deadline.** Sleeping
  in a Lambda is billed wall-clock time; a sleep past the function timeout kills
  the invocation mid-flight and turns one rate-limit into a duplicate execution via
  AWS's async retry. Retry policies (`rest_policy.hpp`) are therefore expressed as
  an explicit wait budget that callers clamp to the deadline via
  `retry_policy_for_deadline`. **Sync interaction paths (ingress, autocomplete)
  must never sleep-retry** — they pass `no_retry`.
- **AD-9 — idempotency defaults to completion markers, not claims.** AWS async
  invoke is at-least-once *and* retries failures. The framework default is *record
  completion after the PATCH succeeds; skip only work already completed* — so
  duplicates of a success skip while a retry of a crash re-runs and the user always
  gets a response. Claim-at-start (`claim_interaction`) is an explicit opt-in for
  non-idempotent side effects only.

The design decisions behind these contracts (AD-1 … AD-9) and the task-by-task
rationale live in
[`docs/plans/tdd-framework-improvement-plan.md`](plans/tdd-framework-improvement-plan.md).
Everything below documents what actually shipped; where the plan and the source
disagree, the source wins.

All functions and types are in `namespace discord_interactions`. `json` is an alias
for `nlohmann::json`.

---

## Interaction ingress & routing

### `interaction.hpp`

Parses inbound interaction JSON: metadata extraction, command-path derivation,
context-menu classification, component-prefix extraction, and the mechanical
name-derivation rules the routers rely on. Depends only on `nlohmann/json` and the
standard library.

```cpp
struct InteractionMetadata {
    std::string application_id, token, guild_id, channel_id, user_id;
};
std::string header_value(const json& headers, const std::string& key);   // case-insensitive
std::string interaction_user_id(const json& interaction);                // member.user.id or user.id
InteractionMetadata metadata(const json& interaction);
std::string application_command_path(const json& interaction);           // "group sub subsub"

enum class ApplicationCommandKind { chat_input = 1, user = 2, message = 3 };
ApplicationCommandKind application_command_kind(const json& interaction); // by data.type; default chat_input
std::string context_menu_route_suffix(const std::string& name);          // "Report User" -> "report-user"
std::string route_suffix_from_path(const std::string& path);             // spaces -> '-'
std::string component_prefix(const std::string& custom_id);              // text before first ':', throws if empty

// Exact-match membership in the generated DISCORD_EPHEMERAL_DEFER_ROUTES CSV.
// Entries are edge-trimmed; "account" never matches an "account link" entry.
bool route_in_csv_allowlist(const std::string& command_path, const std::string& csv);
```

```cpp
const auto meta = discord_interactions::metadata(interaction);
discord_interactions::patch_original_response(meta.application_id, meta.token,
                                              discord_interactions::ephemeral_message("done"));
```

### `routing.hpp`

Resolves a worker Lambda name from a module manifest's route map, accepting both
manifest value forms introduced in T3.2.

```cpp
struct ModuleRoute {
    std::string module, route, function_name, error_mapper_function_name;
};

// "route": "discord-cmd-x"                                       (plain string), or
// "route": {"lambda": "discord-cmd-x", "ephemeral_defer": true} (object; "lambda" required)
// Throws std::runtime_error when the route is missing or resolves to an empty name.
ModuleRoute route_from_manifest_entry(const json& manifest,
                                      const std::string& kind, const std::string& route);
```

```cpp
const auto route = discord_interactions::route_from_manifest_entry(manifest, "commands", "account link");
// route.function_name == "discord-cmd-account-link"
```

### `unknown_route.hpp`

A single dependency-free constant: the user-facing copy the async routers PATCH to
`@original` when a resolved worker Lambda does not exist (T3.4). Surfaced verbatim
to users, so it carries no placeholders and no internal error text.

```cpp
inline constexpr const char* unknown_route_user_copy = "That command isn't available right now.";
```

```cpp
discord_interactions::discord_request(
    "PATCH", discord_interactions::webhook_url(app_id, token, "/messages/@original"),
    json{{"content", discord_interactions::unknown_route_user_copy}, {"flags", 64}},
    discord_interactions::no_retry);
```

### `lambda_client.hpp`

AWS SDK Lambda-client helpers for the routers: warm-client configuration,
async/sync invoke, and the not-found classifier that distinguishes an unregistered
route from any other invoke failure. Pulls in the AWS SDK Lambda client.

```cpp
bool is_function_not_found(const Aws::Client::AWSError<Aws::Lambda::LambdaErrors>& error);
void configure_lambda_client(Aws::Client::ClientConfiguration& config);   // reads AWS_REGION, AWS_LAMBDA_ENDPOINT
void invoke_async(Aws::Lambda::LambdaClient& client, const std::string& function_name,
                  const json& payload, const char* allocation_tag = "DiscordInteractionsInvoke");
json invoke_sync(Aws::Lambda::LambdaClient& client, const std::string& function_name,
                 const json& payload, const char* allocation_tag = "DiscordInteractionsSyncInvoke");
```

```cpp
static Aws::Lambda::LambdaClient client{[]{
    Aws::Client::ClientConfiguration c{}; discord_interactions::configure_lambda_client(c); return c; }()};
discord_interactions::invoke_async(client, "discord-cmd-account-link", interaction);
```

---

## Responses & REST

### `response.hpp`

Interaction-response callback-type constants and the small builders for the values
the gateway returns inline, plus the sync-path `patch_original_response` wrapper.
Re-exports `write_callback`/`discord_api_base_url` from `rest.hpp` for older callers.

```cpp
inline constexpr int ephemeral_flag = 64;
inline constexpr int response_pong = 1, response_channel_message = 4,
    response_deferred_channel_message = 5, response_deferred_update_message = 6,
    response_update_message = 7, response_autocomplete_result = 8, response_modal = 9,
    response_premium_required = 10;  // DEPRECATED by Discord — prefer premium_button

aws::lambda_runtime::invocation_response proxy_response(int status_code, const json& body);
json interaction_response(int type, const json& data = json::object());
json ephemeral_message(const std::string& content);                  // {content, flags:64}
json link_button_row(const std::string& label, const std::string& url);

// Thin wrapper over discord_request + webhook_url. Uses no_retry (single attempt,
// zero sleep) — for the sync interaction paths where sleeping is forbidden (AD-8).
// Async workers wanting rate-limit retries should call discord_request directly.
void patch_original_response(const std::string& application_id,
                             const std::string& interaction_token, const json& message_payload);
```

```cpp
return discord_interactions::proxy_response(
    200, discord_interactions::interaction_response(discord_interactions::response_pong));
```

### `rest_policy.hpp`

Pure retry/rate-limit policy logic for the REST core (T2.1). Standard library +
`nlohmann/json` only — no curl, no AWS SDK — so the unit suite covers every function
here without network access. Also holds the pure webhook-path builders.

```cpp
struct RestRetryPolicy { int max_attempts = 3; long max_total_wait_ms = 8000; };
inline constexpr RestRetryPolicy no_retry{1, 0};                     // AD-8: sync paths

// Clamp the wait budget to the invocation deadline minus a safety margin. Ample
// time -> default 8000ms budget; tight -> remaining-minus-margin; at/past deadline
// -> budget 0 and attempts collapse to 1.
RestRetryPolicy retry_policy_for_deadline(std::chrono::time_point<std::chrono::system_clock> deadline,
                                          long safety_margin_ms = 2000, int max_attempts = 3);

long parse_retry_after(long status, const json& headers, const std::string& body); // body retry_after > header > 1000ms
long backoff_ms(int attempt);                                        // 250 << attempt
std::string join_url(const std::string& base, const std::string& path); // exactly one '/' at the seam
std::string webhook_base_path(const std::string& application_id, const std::string& token);
std::string original_message_path(const std::string& application_id, const std::string& token);
std::string followup_message_path(const std::string& application_id, const std::string& token,
                                  const std::string& message_id);
```

```cpp
const auto policy = discord_interactions::retry_policy_for_deadline(request.get_deadline());
discord_interactions::discord_request("PATCH", url, body, policy);
```

### `rest.hpp`

The rate-limit-aware Discord REST core: executes HTTP via curl with 429/5xx retry
under the `RestRetryPolicy` budget. Owns the base-URL resolution where the
`DISCORD_API_BASE_URL` / `DISCORD_API_VERSION` env overrides enter. Response bodies
and curl details go into internal error context only — never surface them to users.

```cpp
struct RestResponse { long status = 0; std::string body; };
size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata);
std::string discord_api_base_url();  // DISCORD_API_BASE_URL, else https://discord.com/api/v<DISCORD_API_VERSION|10>
std::string webhook_url(const std::string& application_id, const std::string& token,
                        const std::string& suffix);

// 2xx -> RestResponse. 429 -> sleep min(retry_after, remaining budget) and retry.
// 5xx -> backoff_ms retry. Non-429 4xx -> throw immediately. On exhaustion throws
// ModuleError(code="discord_api_error", category=upstream). Takes a full URL.
RestResponse discord_request(const std::string& method, const std::string& url,
                             const json& body = json(), const RestRetryPolicy& policy = {});
```

```cpp
discord_interactions::discord_request(
    "PATCH", discord_interactions::webhook_url(app_id, token, "/messages/@original"),
    json{{"content", "updated"}}, discord_interactions::no_retry);
```

### `webhook_messages.hpp`

Interaction-webhook message lifecycle (T2.2): create followup, fetch/edit/delete
messages on the webhook-token surface — parity with discord.js `followUp()` /
`editReply()` / `deleteReply()`. Runs on **async workers**, so it defaults to the
full `RestRetryPolicy{}` (not `no_retry`); a worker should still clamp that budget
to its deadline via `retry_policy_for_deadline`. Interaction tokens expire after 15
minutes.

```cpp
json create_followup(const std::string& application_id, const std::string& token,
                     const json& message, const RestRetryPolicy& policy = {}); // returns created message
json get_original(const std::string& application_id, const std::string& token,
                  const RestRetryPolicy& policy = {});
void edit_followup(const std::string& application_id, const std::string& token,
                   const std::string& message_id, const json& message, const RestRetryPolicy& policy = {});
void delete_original(const std::string& application_id, const std::string& token,
                     const RestRetryPolicy& policy = {});
void delete_followup(const std::string& application_id, const std::string& token,
                     const std::string& message_id, const RestRetryPolicy& policy = {});
```

```cpp
const json msg = discord_interactions::create_followup(app_id, token, json{{"content", "second message"}});
discord_interactions::delete_followup(app_id, token, msg.value("id", ""));
```

---

## Errors

### `errors.hpp`

The structured error type the whole framework raises instead of matching on
exception text. `ModuleError` carries a stable `code`, a generic `category`, and
separate `safe_context` vs `debug_context` so internal detail never leaks to users.
Use the `MODULE_ERROR(code, category, message)` macro to capture file/line/function.

```cpp
enum class ErrorCategory { validation, auth, upstream, storage, configuration, rate_limited, internal };
const char* error_category_name(ErrorCategory category);

struct ModuleError : std::exception {
    std::string code; ErrorCategory category = ErrorCategory::internal;
    std::string internal_message, file; int line = 0; std::string function;
    json safe_context = json::object(), debug_context = json::object();
    const char* what() const noexcept override;                 // internal_message
    static ModuleError make(std::string code, ErrorCategory category, std::string message,
                            std::string file, int line, std::string function,
                            json safe = json::object(), json debug = json::object());
};
json to_json(const ModuleError& error);
json error_mapper_request(const std::string& module_name, const std::string& route,
                          const json& interaction, const ModuleError& error);

#define MODULE_ERROR(code, category, message) /* ...make(...) with __FILE__, __LINE__, __func__ */
```

```cpp
if (id.empty())
    throw MODULE_ERROR("missing_account_id", discord_interactions::ErrorCategory::validation,
                       "account id option was empty");
```

---

## Limits & builders

### `limits.hpp`

The authoritative Discord limit constants (`namespace limits`) — the single source
of truth every builder clamps against — plus UTF-8-safe truncation.

```cpp
namespace limits {
inline constexpr std::size_t content = 2000, embed_title = 256, embed_description = 4096,
    embed_fields = 25, embed_field_name = 256, embed_field_value = 1024, embed_footer = 2048,
    embed_author = 256, embed_total = 6000, embeds_per_message = 10, action_rows = 5,
    buttons_per_row = 5, select_options = 25, autocomplete_choices = 25, choice_name = 100,
    custom_id = 100, modal_title = 45, text_input_label = 45, text_input_value = 4000,
    components_per_message = 40, text_display_content = 4000;
}
// Never splits a multibyte UTF-8 sequence; appends "…" within the byte budget.
std::string safe_truncate(const std::string& text, std::size_t max_bytes);
```

```cpp
const std::string body = discord_interactions::safe_truncate(json_dump, 1500);
```

### `embed.hpp`

Fluent, limit-enforcing embed builder (discord.js `EmbedBuilder` parity). Every
string setter clamps through `safe_truncate`; `field()` silently drops past the
25-field cap; `build()` enforces the combined 6000-character cap and throws
`ModuleError(code="embed_too_large", validation)` on overflow.

```cpp
class EmbedBuilder {
    EmbedBuilder& title(const std::string&);        EmbedBuilder& description(const std::string&);
    EmbedBuilder& url(const std::string&);          EmbedBuilder& color(uint32_t rgb);
    EmbedBuilder& timestamp_iso8601(const std::string&);
    EmbedBuilder& footer(const std::string& text, const std::string& icon_url = "");
    EmbedBuilder& author(const std::string& name, const std::string& url = "", const std::string& icon_url = "");
    EmbedBuilder& thumbnail(const std::string& url); EmbedBuilder& image(const std::string& url);
    EmbedBuilder& field(const std::string& name, const std::string& value, bool inline_field = false);
    json build() const;
};
```

```cpp
const json embed = discord_interactions::EmbedBuilder{}
    .title("Account").description("Linked").color(0x5865F2).build();
```

### `components.hpp`

Message component (v1) builders: buttons (all styles), string select, entity
selects (user/role/channel), and action-row composition with Discord's row limits.
Emits plain `json`. Non-link buttons and every select clamp `custom_id` to
`limits::custom_id`.

```cpp
enum class ButtonStyle { primary = 1, secondary = 2, success = 3, danger = 4, link = 5 };
json button(ButtonStyle style, const std::string& custom_id_or_url, const std::string& label,
            bool disabled = false, const json& emoji = json());   // link -> url, else custom_id
json select_option(const std::string& label, const std::string& value,
                   const std::string& description = "", bool is_default = false);
json string_select(const std::string& custom_id, const json& options, const std::string& placeholder = "",
                   int min_values = 1, int max_values = 1, bool disabled = false);
json user_select(const std::string& custom_id, const std::string& placeholder = "");
json role_select(const std::string& custom_id, const std::string& placeholder = "");
json channel_select(const std::string& custom_id, const std::string& placeholder = "",
                    const json& channel_types = json::array());
json action_row(const json& components);   // throws too_many_components past 5 buttons / >1 select
json rows(std::initializer_list<json> row_list);  // throws too_many_components past 5 rows
```

```cpp
const json components = discord_interactions::rows({
    discord_interactions::action_row(json::array({
        discord_interactions::button(discord_interactions::ButtonStyle::primary, "vote:yes", "Yes")}))});
```

### `components_v2.hpp`

Components V2 layout builders (message flag `1 << 15`): container / section /
text-display and the surrounding primitives (types 9–14, 17). A CV2 message
**cannot** carry `content` or `embeds`, so `components_v2_message` owns the whole
payload shape and validates the recursive component count against
`limits::components_per_message`.

```cpp
inline constexpr int message_flag_components_v2 = 1 << 15;
json text_display(const std::string& content);
json thumbnail_component(const std::string& media_url, const std::string& description = "", bool spoiler = false);
json section(const json& text_displays, const json& accessory);  // 1..3 text displays, else invalid_section
json media_gallery(const json& items);                           // 1..10 items
json file_component(const std::string& attachment_url, bool spoiler = false);
json separator(bool divider = true, int spacing = 1);            // spacing 1 or 2
json container(const json& components, std::optional<uint32_t> accent_color = std::nullopt, bool spoiler = false);
json components_v2_message(const json& components);              // {flags, components}; throws too_many_components_v2
```

```cpp
const json message = discord_interactions::components_v2_message(json::array({
    discord_interactions::container(json::array({
        discord_interactions::text_display("**Welcome**")}))}));
```

### `modal.hpp`

Modal interaction responses and modal-submit value extraction. Emits Discord's
CURRENT Label-wrapper layout (Label type 18 wrapping a bare input); the deprecated
Action-Row-with-Text-Input layout is never emitted. `modal()` returns the full
`{type:9, data:{...}}` response and accepts only Label (18) / Text Display (10) at
the top level.

```cpp
enum class TextInputStyle { short_input = 1, paragraph = 2 };
json text_input(const std::string& custom_id, TextInputStyle style, bool required = true,
                const std::string& placeholder = "", const std::string& value = "",
                int min_length = 0, int max_length = 0);
json label_component(const std::string& label, const json& child, const std::string& description = "");
json file_upload(const std::string& custom_id, bool required = true);            // type 19
json radio_option(const std::string& label, const std::string& value,
                  const std::string& description = "", bool is_default = false);
json radio_group(const std::string& custom_id, const json& options, bool required = true);      // type 21
json checkbox_group(const std::string& custom_id, const json& options, bool required = true,
                    int min_values = -1, int max_values = -1);                    // type 22
json checkbox(const std::string& custom_id, bool required = false, bool is_default = false);    // type 23
json modal(const std::string& custom_id, const std::string& title, std::initializer_list<json> components);

// Submit-side extraction, recursing through Label wrappers:
std::optional<std::string> modal_value(const json& interaction, const std::string& custom_id);
std::vector<std::string> modal_values(const json& interaction, const std::string& custom_id); // checkbox-group / file ids
std::optional<bool> modal_checked(const json& interaction, const std::string& custom_id);
```

```cpp
const json response = discord_interactions::modal("feedback", "Feedback", {
    discord_interactions::label_component("Message",
        discord_interactions::text_input("body", discord_interactions::TextInputStyle::paragraph))});
```

### `autocomplete.hpp`

Autocomplete response helpers (type 4 → `{type:8}`): build `{name,value}` choices,
assemble a capped/truncated response, filter choices against the user's partial
input (prefix matches before substring, stable), and read the focused option out of
nested subcommand/group option lists.

```cpp
json choice(const std::string& name, const std::string& value);
json choice(const std::string& name, int64_t value);
json choice(const std::string& name, double value);
json autocomplete_response(const json& choices);   // keeps first 25, truncates names to 100 -> {type:8,...}
json filter_choices(const json& choices, const std::string& query);  // case-insensitive; empty query -> unchanged
std::optional<std::string> focused_option_name(const json& interaction);
std::optional<std::string> focused_option_value(const json& interaction);  // numeric values stringified
```

```cpp
const auto q = discord_interactions::focused_option_value(interaction).value_or("");
return discord_interactions::autocomplete_response(
    discord_interactions::filter_choices(all_choices, q));
```

### `custom_id.hpp`

The structured `custom_id` state codec (T1.7) — the framework's only free state
channel (see the architecture contract). Encodes/parses a `prefix[:arg]*` wire
format and enforces the 100-byte budget at encode time.

```cpp
// Throws ModuleError(validation) on empty/':'-containing prefix, ':'-containing arg,
// or encoded length > limits::custom_id (100).
std::string encode_custom_id(const std::string& prefix, const std::vector<std::string>& args);
struct CustomId { std::string prefix; std::vector<std::string> args; };
CustomId parse_custom_id(const std::string& raw);   // throws when the prefix is empty
```

```cpp
const std::string id = discord_interactions::encode_custom_id("vote", {"proposal-42"});
const auto parsed = discord_interactions::parse_custom_id(id);  // {prefix:"vote", args:["proposal-42"]}
```

### `paginator.hpp`

A five-button navigation row (first / prev / counter / next / last) with correct
end-clamping, built on the T1.7 codec. The counter button is always disabled and
carries a non-numeric `"noop"` sentinel arg, so handlers must ignore non-numeric
args — `parse_page_after_prefix` stays strict and throws on them.

```cpp
json paginator_row(const std::string& prefix, size_t page, size_t total_pages);   // one action row of 5 buttons
size_t parse_page_after_prefix(const std::string& custom_id, const std::string& expected_prefix);
```

```cpp
const json row = discord_interactions::paginator_row("results", current_page, total_pages);
const size_t target = discord_interactions::parse_page_after_prefix(interaction_custom_id, "results");
```

### `format.hpp`

The Discord formatting utility belt: snowflake validation/timestamp extraction,
mentions, dynamic `<t:...>` timestamps, markdown escaping, and fenced code blocks
(embedded backticks separated by a U+200B zero-width space so a `` ``` `` can't
close the fence early).

```cpp
inline constexpr int64_t discord_epoch_ms = 1420070400000;      // 2015-01-01T00:00:00Z
bool is_snowflake(const std::string& value);                    // 17..20 digits
int64_t snowflake_timestamp_ms(const std::string& snowflake);   // throws invalid_snowflake
std::string user_mention(const std::string& id);               // <@id>
std::string channel_mention(const std::string& id);            // <#id>
std::string role_mention(const std::string& id);               // <@&id>
enum class TimestampStyle { short_time, long_time, short_date, long_date, short_datetime, long_datetime, relative };
char timestamp_style_letter(TimestampStyle style);
std::string discord_timestamp(int64_t unix_seconds, TimestampStyle style);   // <t:SECONDS:X>
std::string escape_markdown(const std::string& text);           // * _ ~ ` | > # - ; not idempotent
std::string code_block(const std::string& body, const std::string& lang = "");
```

```cpp
const std::string line = discord_interactions::user_mention(user_id) + " joined " +
    discord_interactions::discord_timestamp(created_unix, discord_interactions::TimestampStyle::relative);
```

---

## Reading command options

### `options.hpp`

Typed, throw-free access to CHAT_INPUT command options. `command_options` walks past
subcommand/group wrappers to the innermost option list; the typed getters return
`std::nullopt` on a missing option or type mismatch. Snowflake-valued options are
read via `option_id`; resolved-data lookups return the matching object or `{}`.

```cpp
namespace option_type { /* sub_command=1, sub_command_group=2, string=3, integer=4, boolean=5,
    user=6, channel=7, role=8, mentionable=9, number=10, attachment=11 */ }
json command_options(const json& interaction);
std::optional<std::string> option_string(const json& interaction, const std::string& name);
std::optional<int64_t> option_int(const json& interaction, const std::string& name);   // integers only
std::optional<double> option_number(const json& interaction, const std::string& name);
std::optional<bool> option_bool(const json& interaction, const std::string& name);
std::optional<std::string> option_id(const json& interaction, const std::string& name); // snowflake-typed only
json resolved_user(const json& interaction, const std::string& user_id);
json resolved_member(const json& interaction, const std::string& user_id);
json resolved_channel(const json& interaction, const std::string& channel_id);
json resolved_role(const json& interaction, const std::string& role_id);
json resolved_attachment(const json& interaction, const std::string& attachment_id);
```

```cpp
const std::string name = discord_interactions::option_string(interaction, "name").value_or("");
if (const auto uid = discord_interactions::option_id(interaction, "target"))
    const json user = discord_interactions::resolved_user(interaction, *uid);
```

---

## Premium / entitlements

### `premium.hpp`

Entitlement inspection and premium-upgrade prompts for monetized apps. Discord
delivers active entitlements on every interaction; features are gated by SKU
ownership, and upgrades use a style-6 premium button (Discord renders label + flow
from the SKU). The deprecated PREMIUM_REQUIRED (type 10) callback is recorded in
`response.hpp` for completeness only — prefer `premium_button()`.

```cpp
json entitlements(const json& interaction);                              // the array, or []
bool has_entitlement_for_sku(const json& interaction, const std::string& sku_id,
                             const std::string& now_iso8601 = "");        // "" skips the expiry check
json premium_button(const std::string& sku_id);                          // {type:2, style:6, sku_id}
```

```cpp
if (!discord_interactions::has_entitlement_for_sku(interaction, premium_sku))
    row.push_back(discord_interactions::premium_button(premium_sku));
```

---

## Interaction idempotency

AWS async invocation is at-least-once *and* retries failed runs, so a worker can
receive the same interaction twice. Per **AD-9** the framework default is a
completion marker (record after the PATCH), never a claim — duplicates of a success
skip while retries of a crash re-run and the user always gets a response. Three
headers implement this in layers: an in-process guard, pure request shaping, and the
DynamoDB-executing primitives.

### `idempotency.hpp`

Zero-dependency, in-process LRU completion marker — best-effort dedup for the **same
warm container only**. `was_completed` is a const, side-effect-free check;
`mark_completed` records an id (refreshing recency) and must be called **only after**
the PATCH succeeds. Usage contract: **check → act → PATCH → mark**.

```cpp
class CompletedInteractions {
    explicit CompletedInteractions(size_t capacity = 128);
    bool was_completed(const std::string& interaction_id) const;   // check BEFORE acting; never mutates
    void mark_completed(const std::string& interaction_id);        // ONLY AFTER the PATCH succeeded
};
```

```cpp
static discord_interactions::CompletedInteractions completed{};
if (completed.was_completed(id)) return;      // skip finished work
/* do the work and PATCH @original */
completed.mark_completed(id);                 // only after the PATCH
```

### `idempotency_requests.hpp`

Pure request-shaping for the durable primitives — std + `nlohmann/json` only (no AWS
SDK), so the unit image can exercise it. The stored item is identical for both
primitives (`{interaction_id:{S}, expires_at:{N}}`); only the condition differs.

```cpp
struct ClaimRequestParts { std::string table; json item; std::string condition; };
// Claim (opt-in): condition = "attribute_not_exists(interaction_id)".
ClaimRequestParts build_claim_request(const std::string& table, const std::string& interaction_id,
                                      int64_t now_epoch_s, int64_t ttl_s = 3600);
// Completion marker (default): empty condition -> unconditional put.
ClaimRequestParts build_completion_record(const std::string& table, const std::string& interaction_id,
                                          int64_t now_epoch_s, int64_t ttl_s = 3600);
std::string idempotency_table_from_env();   // DISCORD_IDEMPOTENCY_TABLE, or "" (feature off)
```

```cpp
const std::string table = discord_interactions::idempotency_table_from_env();
const auto parts = discord_interactions::build_completion_record(table, id, now_s);
```

### `idempotency_store.hpp`

The DynamoDB-executing primitives (AD-9 / Phase 5). **Opt-out:** an empty `table`
makes every function a no-op returning the "proceed" answer. **Fail-open:** any
DynamoDB/infra error is logged and swallowed to "proceed" — the only non-proceed
result is a genuine `ConditionalCheckFailedException` on the claim path. Construct
the `DynamoDBClient` **once as a warm global** in `main()`; point it at
`AWS_DYNAMODB_ENDPOINT` for local testing.

```cpp
// Completion marker (DEFAULT): was_completed (GetItem, before) -> act -> PATCH -> record_completion (after).
bool was_completed(Aws::DynamoDB::DynamoDBClient& client, const std::string& table,
                   const std::string& interaction_id);
void record_completion(Aws::DynamoDB::DynamoDBClient& client, const std::string& table,
                       const std::string& interaction_id, int64_t now_epoch_s, int64_t ttl_s = 3600);
// Claim (OPT-IN): conditional put BEFORE acting; false only on ConditionalCheckFailedException.
bool claim_interaction(Aws::DynamoDB::DynamoDBClient& client, const std::string& table,
                       const std::string& interaction_id, int64_t now_epoch_s, int64_t ttl_s = 3600);
```

```cpp
static Aws::DynamoDB::DynamoDBClient ddb{make_warm_config()};
const std::string table = discord_interactions::idempotency_table_from_env();
if (discord_interactions::was_completed(ddb, table, id)) return;   // duplicate of a success
/* act and PATCH @original */
discord_interactions::record_completion(ddb, table, id, now_epoch_s);
```
