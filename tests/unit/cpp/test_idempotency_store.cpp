#include <doctest/doctest.h>

#include <cstdlib>
#include <string>

// T5.2 unit scope: ONLY the pure request builders + env helper live here.
//
// idempotency_requests.hpp is deliberately std/nlohmann-only so the unit test
// image (which links no AWS SDK — see tests/unit/cpp/CMakeLists.txt) can compile
// it. The three EXECUTING functions (was_completed / record_completion /
// claim_interaction) live in idempotency_store.hpp, which pulls in the AWS
// DynamoDB client headers. Their live GetItem/PutItem branches — the actual
// DynamoDB round-trips, ConditionalCheckFailedException handling, and fail-open
// behavior — are integration territory and are exercised by T5.3's mock-DynamoDB
// harness, NOT here. This file only proves the pure request-shaping logic that
// those functions feed into DynamoDB.

#include "discord_interactions/idempotency_requests.hpp"

using discord_interactions::build_claim_request;
using discord_interactions::build_completion_record;
using discord_interactions::ClaimRequestParts;
using discord_interactions::idempotency_table_from_env;
using json = nlohmann::json;

// --- build_claim_request ----------------------------------------------------

TEST_CASE("build_claim_request emits the DynamoDB item with S/N typed strings") {
    const ClaimRequestParts parts = build_claim_request("idem", "abc123", 1000, 3600);

    REQUIRE(parts.item.contains("interaction_id"));
    REQUIRE(parts.item["interaction_id"].contains("S"));
    CHECK(parts.item["interaction_id"]["S"] == "abc123");
    // Snowflake ids are strings under the "S" DynamoDB type descriptor.
    CHECK(parts.item["interaction_id"]["S"].is_string());

    REQUIRE(parts.item.contains("expires_at"));
    REQUIRE(parts.item["expires_at"].contains("N"));
    // DynamoDB numbers are transported as strings under the "N" descriptor.
    CHECK(parts.item["expires_at"]["N"].is_string());
    CHECK(parts.item["expires_at"]["N"] == "4600");
}

TEST_CASE("build_claim_request condition is the conditional attribute_not_exists guard") {
    const ClaimRequestParts parts = build_claim_request("idem", "abc123", 1000, 3600);
    CHECK(parts.condition == "attribute_not_exists(interaction_id)");
}

TEST_CASE("build_claim_request passes the table name through") {
    const ClaimRequestParts parts = build_claim_request("my-table", "id", 1000, 3600);
    CHECK(parts.table == "my-table");
}

TEST_CASE("build_claim_request defaults the TTL to 3600 seconds") {
    const ClaimRequestParts parts = build_claim_request("idem", "id", 1000);
    CHECK(parts.item["expires_at"]["N"] == "4600");
}

TEST_CASE("build_claim_request TTL arithmetic honours a custom window") {
    const ClaimRequestParts parts = build_claim_request("idem", "id", 2000, 900);
    CHECK(parts.item["expires_at"]["N"] == "2900");
}

TEST_CASE("build_claim_request with a zero TTL expires at the current epoch") {
    const ClaimRequestParts parts = build_claim_request("idem", "id", 1500, 0);
    CHECK(parts.item["expires_at"]["N"] == "1500");
}

// --- build_completion_record ------------------------------------------------

TEST_CASE("build_completion_record has an empty (unconditional) condition") {
    const ClaimRequestParts parts = build_completion_record("idem", "abc123", 1000, 3600);
    CHECK(parts.condition == "");
}

TEST_CASE("build_completion_record item shape matches the claim item exactly") {
    const ClaimRequestParts claim = build_claim_request("idem", "abc123", 1000, 3600);
    const ClaimRequestParts record = build_completion_record("idem", "abc123", 1000, 3600);
    // Only the condition differs between the two primitives; the stored item is identical.
    CHECK(record.item == claim.item);
    CHECK(record.table == claim.table);
}

TEST_CASE("build_completion_record passes the table name through") {
    const ClaimRequestParts parts = build_completion_record("completion-table", "id", 1000, 3600);
    CHECK(parts.table == "completion-table");
}

TEST_CASE("build_completion_record TTL arithmetic honours a custom window") {
    const ClaimRequestParts parts = build_completion_record("idem", "id", 500, 60);
    CHECK(parts.item["expires_at"]["N"] == "560");
}

// --- claim vs completion condition contrast ---------------------------------

TEST_CASE("claim is conditional while completion is unconditional") {
    const ClaimRequestParts claim = build_claim_request("idem", "id", 0, 10);
    const ClaimRequestParts record = build_completion_record("idem", "id", 0, 10);
    CHECK_FALSE(claim.condition.empty());
    CHECK(record.condition.empty());
}

// --- idempotency_table_from_env ---------------------------------------------

TEST_CASE("idempotency_table_from_env returns the value when the env var is set") {
    setenv("DISCORD_IDEMPOTENCY_TABLE", "prod-idem", 1);
    CHECK(idempotency_table_from_env() == "prod-idem");
    unsetenv("DISCORD_IDEMPOTENCY_TABLE");
}

TEST_CASE("idempotency_table_from_env returns empty when the env var is unset") {
    unsetenv("DISCORD_IDEMPOTENCY_TABLE");
    CHECK(idempotency_table_from_env() == "");
}

TEST_CASE("idempotency_table_from_env treats an empty env var as opt-out") {
    setenv("DISCORD_IDEMPOTENCY_TABLE", "", 1);
    CHECK(idempotency_table_from_env() == "");
    unsetenv("DISCORD_IDEMPOTENCY_TABLE");
}
