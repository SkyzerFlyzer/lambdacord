#include <doctest/doctest.h>

#include <chrono>
#include <string>

#include "discord_interactions/rest_policy.hpp"

using discord_interactions::backoff_ms;
using discord_interactions::is_retryable_status;
using discord_interactions::join_url;
using discord_interactions::no_retry;
using discord_interactions::parse_retry_after;
using discord_interactions::redact_webhook_token;
using discord_interactions::RestRetryPolicy;
using discord_interactions::retry_policy_for_deadline;
using json = nlohmann::json;

// --- parse_retry_after ------------------------------------------------------

TEST_CASE("parse_retry_after prefers float retry_after seconds in the JSON body") {
    const json headers = json::object();
    CHECK(parse_retry_after(429, headers, R"({"retry_after": 1.5})") == 1500);
}

TEST_CASE("parse_retry_after converts integer retry_after body seconds to ms") {
    const json headers = json::object();
    CHECK(parse_retry_after(429, headers, R"({"retry_after": 2})") == 2000);
}

TEST_CASE("parse_retry_after handles sub-second float retry_after values") {
    const json headers = json::object();
    CHECK(parse_retry_after(429, headers, R"({"retry_after": 0.25})") == 250);
}

TEST_CASE("parse_retry_after prefers the JSON body over the Retry-After header") {
    const json headers = {{"Retry-After", "9"}};
    CHECK(parse_retry_after(429, headers, R"({"retry_after": 1.0})") == 1000);
}

TEST_CASE("parse_retry_after falls back to the Retry-After header (integer seconds)") {
    const json headers = {{"Retry-After", "3"}};
    CHECK(parse_retry_after(429, headers, "") == 3000);
}

TEST_CASE("parse_retry_after header lookup is case-insensitive") {
    const json lower = {{"retry-after", "2"}};
    CHECK(parse_retry_after(429, lower, "") == 2000);

    const json upper = {{"RETRY-AFTER", "4"}};
    CHECK(parse_retry_after(429, upper, "") == 4000);
}

TEST_CASE("parse_retry_after returns the documented 1000ms default when nothing is present") {
    const json headers = json::object();
    CHECK(parse_retry_after(429, headers, "") == 1000);
}

TEST_CASE("parse_retry_after falls back to the header when the body is malformed JSON") {
    const json headers = {{"Retry-After", "5"}};
    CHECK(parse_retry_after(429, headers, "not json at all {") == 5000);
}

TEST_CASE("parse_retry_after defaults when the body is malformed and no header exists") {
    const json headers = json::object();
    CHECK(parse_retry_after(429, headers, "<html>oops</html>") == 1000);
}

TEST_CASE("parse_retry_after defaults when the Retry-After header is not numeric") {
    const json headers = {{"Retry-After", "soon"}};
    CHECK(parse_retry_after(429, headers, "") == 1000);
}

TEST_CASE("parse_retry_after ignores a JSON body whose retry_after is not a number") {
    const json headers = {{"Retry-After", "2"}};
    CHECK(parse_retry_after(429, headers, R"({"retry_after": "later"})") == 2000);
}

// --- backoff_ms -------------------------------------------------------------

TEST_CASE("backoff_ms yields the documented 250/500/1000 sequence for attempts 0..2") {
    CHECK(backoff_ms(0) == 250);
    CHECK(backoff_ms(1) == 500);
    CHECK(backoff_ms(2) == 1000);
}

TEST_CASE("backoff_ms keeps doubling beyond the default attempt count") {
    CHECK(backoff_ms(3) == 2000);
}

// --- retry policies ---------------------------------------------------------

TEST_CASE("no_retry is a single attempt with a zero wait budget (AD-8 sync paths)") {
    CHECK(no_retry.max_attempts == 1);
    CHECK(no_retry.max_total_wait_ms == 0);
    // The per-attempt transport ceiling keeps its default: a single sync
    // attempt still gets the full 10s transport window.
    CHECK(no_retry.max_attempt_ms == 10000);
}

TEST_CASE("RestRetryPolicy defaults to 3 attempts and an 8000ms wait budget") {
    const RestRetryPolicy policy{};
    CHECK(policy.max_attempts == 3);
    CHECK(policy.max_total_wait_ms == 8000);
    CHECK(policy.max_attempt_ms == 10000);
}

TEST_CASE("retry_policy_for_deadline grants the full default budget with ample time") {
    const auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(60);
    const RestRetryPolicy policy = retry_policy_for_deadline(deadline);
    CHECK(policy.max_attempts == 3);
    CHECK(policy.max_total_wait_ms == 8000);
    // Ample time: the per-attempt transport ceiling stays at the 10000ms cap.
    CHECK(policy.max_attempt_ms == 10000);
}

TEST_CASE("retry_policy_for_deadline clamps a tight budget to remaining minus margin") {
    const auto deadline =
        std::chrono::system_clock::now() + std::chrono::milliseconds(5000);
    const RestRetryPolicy policy = retry_policy_for_deadline(deadline, 2000);
    // Remaining is ~5000ms, margin 2000ms → budget ~3000ms. Allow slack for the
    // wall-clock time between building the deadline and calling the function.
    CHECK(policy.max_total_wait_ms <= 3000);
    CHECK(policy.max_total_wait_ms >= 2500);
    CHECK(policy.max_attempts == 3);
    // AD-8: a single attempt must not sail past the deadline either — the
    // transport ceiling clamps to remaining-minus-margin (~3000ms here).
    CHECK(policy.max_attempt_ms <= 3000);
    CHECK(policy.max_attempt_ms >= 2500);
}

TEST_CASE("retry_policy_for_deadline collapses to one attempt when the deadline passed") {
    const auto deadline = std::chrono::system_clock::now() - std::chrono::seconds(1);
    const RestRetryPolicy policy = retry_policy_for_deadline(deadline);
    CHECK(policy.max_total_wait_ms == 0);
    CHECK(policy.max_attempts == 1);
    // Past the deadline the transport ceiling clamps to the 1000ms floor: the
    // single mandatory attempt still gets a minimal transport window.
    CHECK(policy.max_attempt_ms == 1000);
}

TEST_CASE("retry_policy_for_deadline collapses when only the safety margin remains") {
    const auto deadline =
        std::chrono::system_clock::now() + std::chrono::milliseconds(1500);
    const RestRetryPolicy policy = retry_policy_for_deadline(deadline, 2000);
    CHECK(policy.max_total_wait_ms == 0);
    CHECK(policy.max_attempts == 1);
    CHECK(policy.max_attempt_ms == 1000);
}

// --- is_retryable_status ------------------------------------------------------

TEST_CASE("is_retryable_status retries only 429 and 5xx") {
    CHECK(is_retryable_status(429));
    CHECK(is_retryable_status(500));
    CHECK(is_retryable_status(502));
    CHECK(is_retryable_status(503));
}

TEST_CASE("is_retryable_status never retries 1xx/2xx/3xx or non-429 4xx") {
    CHECK_FALSE(is_retryable_status(100));
    CHECK_FALSE(is_retryable_status(200));
    CHECK_FALSE(is_retryable_status(204));
    CHECK_FALSE(is_retryable_status(301));
    CHECK_FALSE(is_retryable_status(304));
    CHECK_FALSE(is_retryable_status(400));
    CHECK_FALSE(is_retryable_status(403));
    CHECK_FALSE(is_retryable_status(404));
}

// --- redact_webhook_token ----------------------------------------------------

TEST_CASE("redact_webhook_token replaces the token segment of a webhook URL") {
    CHECK(redact_webhook_token(
              "https://discord.com/api/v10/webhooks/1234567890/aW50ZXJhY3Rpb24tdG9rZW4") ==
          "https://discord.com/api/v10/webhooks/1234567890/***");
}

TEST_CASE("redact_webhook_token preserves the trailing path after the token") {
    CHECK(redact_webhook_token("https://discord.com/api/v10/webhooks/1234567890/"
                               "aW50ZXJhY3Rpb24tdG9rZW4/messages/@original") ==
          "https://discord.com/api/v10/webhooks/1234567890/***/messages/@original");
    CHECK(redact_webhook_token(
              "http://localhost:19001/api/v10/webhooks/app-unknown/tok-unknown/messages/42") ==
          "http://localhost:19001/api/v10/webhooks/app-unknown/***/messages/42");
}

TEST_CASE("redact_webhook_token returns non-webhook URLs unchanged") {
    CHECK(redact_webhook_token("https://discord.com/api/v10/channels/1/messages") ==
          "https://discord.com/api/v10/channels/1/messages");
    CHECK(redact_webhook_token("") == "");
    // /webhooks/ with no token segment after the application id: nothing to
    // redact, returned unchanged.
    CHECK(redact_webhook_token("https://discord.com/api/v10/webhooks/1234567890") ==
          "https://discord.com/api/v10/webhooks/1234567890");
}

TEST_CASE("retry_policy_for_deadline honors a caller-supplied max_attempts") {
    const auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(60);
    const RestRetryPolicy policy = retry_policy_for_deadline(deadline, 2000, 5);
    CHECK(policy.max_attempts == 5);
}

// --- join_url ---------------------------------------------------------------

TEST_CASE("join_url inserts exactly one slash when neither side has one") {
    CHECK(join_url("https://discord.com/api/v10", "webhooks/1/t") ==
          "https://discord.com/api/v10/webhooks/1/t");
}

TEST_CASE("join_url collapses a trailing plus leading slash to one") {
    CHECK(join_url("https://discord.com/api/v10/", "/webhooks/1/t") ==
          "https://discord.com/api/v10/webhooks/1/t");
}

TEST_CASE("join_url keeps a single slash when only the base has a trailing slash") {
    CHECK(join_url("https://discord.com/api/v10/", "webhooks/1/t") ==
          "https://discord.com/api/v10/webhooks/1/t");
}

TEST_CASE("join_url keeps a single slash when only the path has a leading slash") {
    CHECK(join_url("https://discord.com/api/v10", "/messages/@original") ==
          "https://discord.com/api/v10/messages/@original");
}

TEST_CASE("join_url returns the base unchanged for an empty path") {
    CHECK(join_url("https://discord.com/api/v10", "") ==
          "https://discord.com/api/v10");
}
