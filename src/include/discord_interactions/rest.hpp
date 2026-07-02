#pragma once

// Rate-limit-aware Discord REST core for the webhook-token surface (T2.1).
//
// Executes HTTP requests via curl with 429/5xx retry, the equivalent of
// discord.js's REST manager for the slice of the API this framework uses. The
// pure retry/parse logic (RestRetryPolicy, parse_retry_after, backoff_ms,
// join_url, retry_policy_for_deadline) lives in rest_policy.hpp so unit tests
// cover it without curl/AWS include paths; this header owns the network side.
//
// AD-8 — sleeping is billed Lambda wall-clock time. Retrying with a sleep is
// acceptable on async workers, whose alternative is a failed invoke and a full
// re-execution via AWS's async retry. It is FORBIDDEN on sync interaction
// paths (ingress, autocomplete): pass no_retry there. Async workers should
// derive their policy from the invocation deadline with
// retry_policy_for_deadline(request.deadline) so a sleep never outlives the
// function timeout.
//
// max_total_wait_ms is a genuine TIME budget, not just a sleep budget: each
// attempt's transport wall time (bounded per attempt by max_attempt_ms via
// CURLOPT_TIMEOUT_MS) is charged against it alongside retry sleeps, so
// attempts and sleeps draw from one pool. The first attempt always runs; a
// retry that no longer fits throws instead. Total wall time is therefore
// bounded by roughly max_attempt_ms (first attempt) + max_total_wait_ms —
// RestRetryPolicy{3, 8000} can no longer stack 3×10s transports on top of 8s
// of sleeps and blow past a 30s worker timeout into the duplicate execution
// AD-8 forbids.
//
// Base URL: discord_request takes a full URL — env overrides
// (DISCORD_API_BASE_URL / DISCORD_API_VERSION) enter only through the URL the
// caller passes, normally built with webhook_url()/discord_api_base_url().

#include <nlohmann/json.hpp>

#include <curl/curl.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include "discord_interactions/errors.hpp"
#include "discord_interactions/rest_policy.hpp"

namespace discord_interactions {

using json = nlohmann::json;

struct RestResponse {
    long status = 0;
    std::string body{};
};

inline size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = reinterpret_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

// Discord REST base URL: DISCORD_API_BASE_URL wins outright (integration
// harness override), else https://discord.com/api/v<DISCORD_API_VERSION>
// (default v10).
inline std::string discord_api_base_url() {
    const char* base_url_env = std::getenv("DISCORD_API_BASE_URL");
    if (base_url_env != nullptr && *base_url_env != '\0') {
        return base_url_env;
    }

    const char* version_env = std::getenv("DISCORD_API_VERSION");
    const std::string version =
        (version_env == nullptr || *version_env == '\0') ? "10" : version_env;
    return "https://discord.com/api/v" + version;
}

// Full webhook-token URL: discord_api_base_url() + /webhooks/APP/TOKEN +
// suffix (e.g. "/messages/@original"), with exactly one '/' at each join.
inline std::string webhook_url(const std::string& application_id, const std::string& token,
                               const std::string& suffix) {
    const std::string webhook_path =
        join_url(join_url("webhooks", application_id), token);
    return join_url(join_url(discord_api_base_url(), webhook_path), suffix);
}

namespace rest_detail {

// curl header callback: collects response headers into a json object keyed by
// header name (as sent), values trimmed of surrounding whitespace/CRLF.
// parse_retry_after does its own case-insensitive lookup.
inline size_t header_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* headers = reinterpret_cast<json*>(userdata);
    const std::string line(ptr, size * nmemb);
    const std::string::size_type colon = line.find(':');
    if (colon != std::string::npos) {
        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        const auto trim = [](std::string& s) {
            const char* whitespace = " \t\r\n";
            const std::string::size_type begin = s.find_first_not_of(whitespace);
            const std::string::size_type end = s.find_last_not_of(whitespace);
            s = (begin == std::string::npos) ? std::string{}
                                             : s.substr(begin, end - begin + 1);
        };
        trim(key);
        trim(value);
        if (!key.empty()) {
            (*headers)[key] = value;
        }
    }
    return size * nmemb;
}

struct AttemptResult {
    long status = 0;
    std::string body{};
    json headers = json::object();
};

// One curl round trip, capped at max_attempt_ms of transport time (AD-8).
// Throws ModuleError(upstream) on transport failure.
inline AttemptResult perform_once(const std::string& method, const std::string& url,
                                  const std::string& request_body, bool has_body,
                                  long max_attempt_ms) {
    using curl_handle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
    using curl_header_list = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;

    curl_handle curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) {
        throw MODULE_ERROR("discord_api_error", ErrorCategory::upstream,
                           "curl_easy_init failed");
    }

    curl_slist* raw_headers =
        curl_slist_append(nullptr, "User-Agent: DiscordBot (lambdacord, 1.0)");
    curl_header_list headers(raw_headers, &curl_slist_free_all);
    if (!headers) {
        throw MODULE_ERROR("discord_api_error", ErrorCategory::upstream,
                           "curl_slist_append failed");
    }
    if (has_body) {
        raw_headers = curl_slist_append(headers.get(), "Content-Type: application/json");
        if (raw_headers == nullptr) {
            throw MODULE_ERROR("discord_api_error", ErrorCategory::upstream,
                               "curl_slist_append failed");
        }
        headers.release();
        headers.reset(raw_headers);
    }

    AttemptResult result{};

    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, method.c_str());
    if (has_body) {
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, request_body.c_str());
    }
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &result.body);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &result.headers);
    // AD-8: the policy's per-attempt transport ceiling, not a fixed 10s — so a
    // deadline-derived policy can shrink a single attempt below the remaining
    // invocation time. Connect phase stays at 5s but never above the ceiling.
    const long attempt_timeout_ms = max_attempt_ms > 0 ? max_attempt_ms : 1000;
    const long connect_timeout_ms =
        attempt_timeout_ms < 5000 ? attempt_timeout_ms : 5000;
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, attempt_timeout_ms);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, connect_timeout_ms);
    curl_easy_setopt(curl.get(), CURLOPT_CAINFO, "/etc/pki/tls/certs/ca-bundle.crt");

    const CURLcode res = curl_easy_perform(curl.get());
    if (res != CURLE_OK) {
        throw MODULE_ERROR("discord_api_error", ErrorCategory::upstream,
                           std::string("Discord ") + method +
                               " transport failure: " + curl_easy_strerror(res));
    }

    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &result.status);
    return result;
}

}  // namespace rest_detail

// Executes `method url` with retries per `policy` under AD-8 time-budget
// semantics — max_total_wait_ms is one pool that BOTH retry sleeps and each
// attempt's transport wall time (measured with steady_clock) draw from, and
// every attempt's transport is capped at policy.max_attempt_ms
// (CURLOPT_TIMEOUT_MS), so total wall time is bounded by roughly
// max_attempt_ms (the always-run first attempt) + max_total_wait_ms:
//   - 2xx returns RestResponse{status, body}.
//   - 429 sleeps parse_retry_after(...) and retries; 5xx retries with
//     backoff_ms(attempt). A retry (sleep or next attempt) that no longer
//     fits in the remaining budget is NOT taken — throws instead. The first
//     attempt always runs regardless of budget.
//   - Every other status — 1xx/3xx and non-429 4xx — is deterministic, not
//     transient (is_retryable_status, rest_policy.hpp): throws immediately,
//     status in safe_context.
//   - After attempt/budget exhaustion throws
//     ModuleError(code="discord_api_error", category=upstream).
// Sends User-Agent: DiscordBot (lambdacord, 1.0) on every request and
// Content-Type: application/json when body is non-null. Takes a full URL —
// callers build it (webhook_url / discord_api_base_url), which is where the
// DISCORD_API_BASE_URL env override applies.
// Response bodies/curl details go into internal_message/debug_context only —
// never surface them to Discord users (CLAUDE.md error-copy rule). URLs are
// stored token-redacted (redact_webhook_token): the token segment of a
// webhook URL is a live credential for the interaction's 15-minute lifetime
// and must never land in error contexts or logs.
inline RestResponse discord_request(const std::string& method, const std::string& url,
                                    const json& body = json(),
                                    const RestRetryPolicy& policy = {}) {
    const bool has_body = !body.is_null();
    const std::string request_body = has_body ? body.dump() : std::string{};
    const int max_attempts = policy.max_attempts > 0 ? policy.max_attempts : 1;
    long remaining_budget_ms = policy.max_total_wait_ms > 0 ? policy.max_total_wait_ms : 0;

    long last_status = 0;
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        const auto attempt_start = std::chrono::steady_clock::now();
        const rest_detail::AttemptResult result = rest_detail::perform_once(
            method, url, request_body, has_body, policy.max_attempt_ms);
        // AD-8: transport wall time and retry sleeps draw from one pool —
        // charge this attempt's elapsed time against the remaining budget.
        const long attempt_elapsed_ms = static_cast<long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - attempt_start)
                .count());
        remaining_budget_ms -= attempt_elapsed_ms;
        if (remaining_budget_ms < 0) {
            remaining_budget_ms = 0;
        }
        last_status = result.status;

        if (result.status >= 200 && result.status < 300) {
            RestResponse response{};
            response.status = result.status;
            response.body = result.body;
            return response;
        }

        if (!is_retryable_status(result.status)) {
            throw ModuleError::make(
                "discord_api_error", ErrorCategory::upstream,
                "Discord " + method + " failed with HTTP " + std::to_string(result.status),
                __FILE__, __LINE__, __func__,
                json{{"status", result.status}},
                json{{"body", result.body}, {"url", redact_webhook_token(url)}});
        }

        // 429 or 5xx: retryable if attempts and time budget remain.
        const bool last_attempt = (attempt + 1 >= max_attempts);
        if (!last_attempt) {
            const long wait_ms = (result.status == 429)
                                     ? parse_retry_after(result.status, result.headers,
                                                         result.body)
                                     : backoff_ms(attempt);
            if (wait_ms <= remaining_budget_ms) {
                std::cerr << "discord_request: HTTP " << result.status << " on " << method
                          << ", retrying in " << wait_ms << "ms (attempt " << (attempt + 1)
                          << "/" << max_attempts << ")" << std::endl;
                std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
                remaining_budget_ms -= wait_ms;
                continue;
            }
            // AD-8: the retry (sleep + another attempt) no longer fits in the
            // remaining time budget — fail instead of overrunning the deadline.
        }
        break;
    }

    throw ModuleError::make(
        "discord_api_error", ErrorCategory::upstream,
        "Discord " + method + " failed with HTTP " + std::to_string(last_status) +
            " after retries were exhausted",
        __FILE__, __LINE__, __func__,
        json{{"status", last_status}},
        json{{"url", redact_webhook_token(url)}});
}

}  // namespace discord_interactions
