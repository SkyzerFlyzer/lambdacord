#include <doctest/doctest.h>

#include <chrono>
#include <string>

#include "discord_interactions/rest_policy.hpp"

using discord_interactions::backoff_ms;
using discord_interactions::join_url;
using discord_interactions::no_retry;
using discord_interactions::parse_retry_after;
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
}

TEST_CASE("RestRetryPolicy defaults to 3 attempts and an 8000ms wait budget") {
    const RestRetryPolicy policy{};
    CHECK(policy.max_attempts == 3);
    CHECK(policy.max_total_wait_ms == 8000);
}

TEST_CASE("retry_policy_for_deadline grants the full default budget with ample time") {
    const auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(60);
    const RestRetryPolicy policy = retry_policy_for_deadline(deadline);
    CHECK(policy.max_attempts == 3);
    CHECK(policy.max_total_wait_ms == 8000);
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
}

TEST_CASE("retry_policy_for_deadline collapses to one attempt when the deadline passed") {
    const auto deadline = std::chrono::system_clock::now() - std::chrono::seconds(1);
    const RestRetryPolicy policy = retry_policy_for_deadline(deadline);
    CHECK(policy.max_total_wait_ms == 0);
    CHECK(policy.max_attempts == 1);
}

TEST_CASE("retry_policy_for_deadline collapses when only the safety margin remains") {
    const auto deadline =
        std::chrono::system_clock::now() + std::chrono::milliseconds(1500);
    const RestRetryPolicy policy = retry_policy_for_deadline(deadline, 2000);
    CHECK(policy.max_total_wait_ms == 0);
    CHECK(policy.max_attempts == 1);
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
