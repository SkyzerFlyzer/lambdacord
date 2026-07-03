#pragma once

// Pure retry/rate-limit policy logic for the Discord REST core (T2.1).
//
// This header deliberately includes ONLY the standard library and
// nlohmann/json — no curl, no AWS SDK — so the unit test suite
// (tests/unit/cpp/, which compiles without curl/AWS include paths) can cover
// every function here without network access. The curl-executing side lives in
// rest.hpp, which includes this header.
//
// AD-8: all in-Lambda waiting is bounded by the invocation deadline. Sleeping
// in a Lambda is billed wall-clock time — tolerable on async workers, but a
// sleep past the function timeout kills the invocation mid-flight and turns
// one rate limit into a duplicate execution via AWS's async retry. Retry
// policies are therefore expressed as an explicit wait budget that callers
// clamp to the invocation deadline (retry_policy_for_deadline). Sync
// interaction paths (ingress, autocomplete) must never sleep-retry — they use
// no_retry.

#include <nlohmann/json.hpp>

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>

namespace discord_interactions {

using json = nlohmann::json;

// Retry envelope for discord_request (rest.hpp). max_attempts counts total
// tries (first attempt included). max_total_wait_ms is a genuine TIME budget
// (AD-8): both retry sleeps AND each attempt's transport wall-clock time are
// charged against it — a retry (sleep or next attempt) that no longer fits in
// the remaining budget is not taken, the request fails instead. The first
// attempt always runs regardless of budget. max_attempt_ms is the per-attempt
// transport ceiling (curl CURLOPT_TIMEOUT_MS), so total wall time is bounded by
// roughly max_attempt_ms (the always-run first attempt) + max_total_wait_ms —
// never attempts*transport + sleeps unbounded by the budget.
struct RestRetryPolicy {
    int max_attempts = 3;
    long max_total_wait_ms = 8000;
    long max_attempt_ms = 10000;
};

// AD-8: sync interaction paths (ingress, autocomplete) must use this — a
// single attempt with zero sleep (max_attempt_ms keeps its 10000ms default;
// the single attempt still gets a full transport window). They already spend
// their 3-second Discord budget on up to two cold starts.
inline constexpr RestRetryPolicy no_retry{1, 0};

// Minimum transport window a retry attempt must have left after its sleep.
// discord_request only takes a retry when sleep + this floor still fit the
// remaining budget, and it shrinks the retry's transport ceiling to whatever
// budget remains — so a sleep can never consume the whole pool and still admit
// a full-ceiling attempt that overruns the deadline (AD-8).
inline constexpr long min_retry_attempt_ms = 250;

// AD-8: clamp the wait budget to the invocation deadline minus a safety
// margin, so a retry never outlives the function timeout. The deadline comes
// from aws::lambda_runtime::invocation_request::deadline. With ample time the
// default 8000ms budget applies; with tight time the budget shrinks to
// remaining-minus-margin; at or past the deadline the budget is zero and the
// attempts collapse to one (nothing to wait with, so retrying is pointless).
// The per-attempt transport ceiling (max_attempt_ms) also clamps to
// remaining-minus-margin — floor 1000ms (the mandatory first attempt needs a
// minimal window), cap 10000ms (the default) — so a single slow attempt cannot
// sail past the deadline either. Combined with discord_request charging
// transport time against the wait budget, total wall time stays around
// max_attempt_ms + max_total_wait_ms ≤ remaining time.
inline RestRetryPolicy retry_policy_for_deadline(
    std::chrono::time_point<std::chrono::system_clock> deadline,
    long safety_margin_ms = 2000, int max_attempts = 3) {
    RestRetryPolicy policy{};
    policy.max_attempts = max_attempts;

    const auto now = std::chrono::system_clock::now();
    const long remaining_ms = static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());

    long budget_ms = remaining_ms - safety_margin_ms;
    if (budget_ms < 0) {
        budget_ms = 0;
    }
    const RestRetryPolicy defaults{};
    if (budget_ms > defaults.max_total_wait_ms) {
        budget_ms = defaults.max_total_wait_ms;
    }

    policy.max_total_wait_ms = budget_ms;
    if (budget_ms == 0) {
        policy.max_attempts = 1;
    }

    constexpr long attempt_floor_ms = 1000;
    long attempt_ms = remaining_ms - safety_margin_ms;
    if (attempt_ms < attempt_floor_ms) {
        attempt_ms = attempt_floor_ms;
    }
    if (attempt_ms > defaults.max_attempt_ms) {
        attempt_ms = defaults.max_attempt_ms;
    }
    policy.max_attempt_ms = attempt_ms;
    return policy;
}

// True when an HTTP status outside 2xx is worth a sleep-retry: 429 (rate
// limit, Discord tells us how long) and 5xx (transient upstream failure).
// Everything else — 1xx/3xx (a Discord API request should never legitimately
// produce these; retrying cannot fix a redirect) and non-429 4xx (the request
// itself is wrong) — is deterministic and must fail immediately. Pure.
inline bool is_retryable_status(long status) {
    return status == 429 || status >= 500;
}

// Redacts the live interaction token from a webhook URL so it can be stored in
// ModuleError debug_context / logs without leaking a credential that stays
// usable for 15 minutes. Replaces the path segment after /webhooks/<app_id>/
// with "***", preserving any trailing path (e.g. /messages/@original). URLs
// without that pattern are returned unchanged. Pure.
inline std::string redact_webhook_token(const std::string& url) {
    constexpr const char* marker = "/webhooks/";
    const std::string::size_type marker_pos = url.find(marker);
    if (marker_pos == std::string::npos) {
        return url;
    }
    const std::string::size_type app_begin = marker_pos + std::string(marker).size();
    const std::string::size_type app_end = url.find('/', app_begin);
    if (app_end == std::string::npos || app_end == app_begin) {
        return url;  // no token segment after the application id
    }
    const std::string::size_type token_begin = app_end + 1;
    const std::string::size_type token_end = url.find('/', token_begin);
    const std::string::size_type token_len =
        (token_end == std::string::npos ? url.size() : token_end) - token_begin;
    if (token_len == 0) {
        return url;  // empty token segment — nothing to redact
    }
    std::string redacted = url;
    redacted.replace(token_begin, token_len, "***");
    return redacted;
}

// How long a 429 asks us to wait, in milliseconds. Preference order:
//   1. JSON response body {"retry_after": <float seconds>} — Discord's
//      rate-limit payload.
//   2. Retry-After response header (integer seconds), looked up
//      case-insensitively in the headers json object.
//   3. A documented default of 1000ms when neither is usable.
// `status` is the HTTP status the values came from; callers feed 429
// responses. Pure — safe to unit test without network.
inline long parse_retry_after(long status, const json& headers, const std::string& body) {
    static_cast<void>(status);  // Part of the documented signature; the parse
                                // itself does not branch on the status code.
    constexpr long default_retry_after_ms = 1000;

    // 1. JSON body {"retry_after": <float seconds>}.
    const json parsed_body = json::parse(body, nullptr, /*allow_exceptions=*/false);
    if (parsed_body.is_object() && parsed_body.contains("retry_after") &&
        parsed_body["retry_after"].is_number()) {
        const double seconds = parsed_body["retry_after"].get<double>();
        if (seconds >= 0.0) {
            return std::lround(seconds * 1000.0);
        }
    }

    // 2. Retry-After header, case-insensitive key lookup, integer seconds.
    for (auto it = headers.begin(); it != headers.end(); ++it) {
        std::string key = it.key();
        for (char& c : key) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (key != "retry-after" || !it.value().is_string()) {
            continue;
        }
        const std::string value = it.value().get<std::string>();
        char* end = nullptr;
        const long seconds = std::strtol(value.c_str(), &end, 10);
        if (end != value.c_str() && seconds >= 0) {
            return seconds * 1000;
        }
    }

    // 3. Documented default.
    return default_retry_after_ms;
}

// Exponential backoff for 5xx retries: 250ms * 2^attempt, attempt 0-based
// (250, 500, 1000, ...). Pure.
inline long backoff_ms(int attempt) {
    return 250L << attempt;
}

// Joins a base URL and a path with exactly one '/' at the seam, regardless of
// trailing/leading slashes on either side. An empty path returns the base
// unchanged. Pure — used by webhook_url (rest.hpp) and unit-testable here.
inline std::string join_url(const std::string& base, const std::string& path) {
    if (path.empty()) {
        return base;
    }
    std::string joined = base;
    while (!joined.empty() && joined.back() == '/') {
        joined.pop_back();
    }
    std::string::size_type first = 0;
    while (first < path.size() && path[first] == '/') {
        ++first;
    }
    joined += '/';
    joined.append(path, first, std::string::npos);
    return joined;
}

// --- Interaction-webhook path helpers (T2.2) --------------------------------
//
// Pure, network-free builders for the webhook-token message routes. They live
// here (not webhook_messages.hpp) so the unit suite, which compiles without
// curl/AWS include paths, can assert URL construction directly. The executing
// lifecycle functions in webhook_messages.hpp compose these with a base URL —
// join_url(discord_api_base_url(), <path>) — collapsing every seam to exactly
// one '/', matching webhook_url()'s conventions in rest.hpp.

// "/webhooks/APP/TOKEN" — the interaction-webhook base path.
inline std::string webhook_base_path(const std::string& application_id,
                                     const std::string& token) {
    return "/webhooks/" + application_id + "/" + token;
}

// "/webhooks/APP/TOKEN/messages/@original" — the original interaction response.
inline std::string original_message_path(const std::string& application_id,
                                         const std::string& token) {
    return webhook_base_path(application_id, token) + "/messages/@original";
}

// "/webhooks/APP/TOKEN/messages/MSG" — a specific followup/message id. An empty
// message_id yields the collection path with a trailing '/'; callers must
// supply a real id.
inline std::string followup_message_path(const std::string& application_id,
                                         const std::string& token,
                                         const std::string& message_id) {
    return webhook_base_path(application_id, token) + "/messages/" + message_id;
}

}  // namespace discord_interactions
