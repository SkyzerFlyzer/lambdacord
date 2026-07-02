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
// tries (first attempt included); max_total_wait_ms is the cumulative sleep
// budget across all retries — a sleep that would exceed the remaining budget
// is not taken, the request fails instead.
struct RestRetryPolicy {
    int max_attempts = 3;
    long max_total_wait_ms = 8000;
};

// AD-8: sync interaction paths (ingress, autocomplete) must use this — a
// single attempt with zero sleep. They already spend their 3-second Discord
// budget on up to two cold starts.
inline constexpr RestRetryPolicy no_retry{1, 0};

// AD-8: clamp the wait budget to the invocation deadline minus a safety
// margin, so a retry never outlives the function timeout. The deadline comes
// from aws::lambda_runtime::invocation_request::deadline. With ample time the
// default 8000ms budget applies; with tight time the budget shrinks to
// remaining-minus-margin; at or past the deadline the budget is zero and the
// attempts collapse to one (nothing to wait with, so retrying is pointless).
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
    const long default_budget_ms = RestRetryPolicy{}.max_total_wait_ms;
    if (budget_ms > default_budget_ms) {
        budget_ms = default_budget_ms;
    }

    policy.max_total_wait_ms = budget_ms;
    if (budget_ms == 0) {
        policy.max_attempts = 1;
    }
    return policy;
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

}  // namespace discord_interactions
