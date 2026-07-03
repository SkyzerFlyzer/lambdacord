#pragma once

// Pure request-shaping for the durable (DynamoDB) interaction-dedup primitives
// (AD-9 / Phase 5). This header is std + nlohmann::json only — NO AWS SDK — so
// the unit-test image (which links no SDK) can compile and exercise it. The
// executing functions that actually talk to DynamoDB live in the sibling
// idempotency_store.hpp, which includes this header plus the AWS DynamoDB
// client headers.
//
// AD-9 decision table (summary — the full table is in the Phase 5 plan section):
//
//   Primitive            | Mechanism                              | Guarantee
//   ---------------------|----------------------------------------|--------------------------
//   Completion marker    | GetItem check, then UNCONDITIONAL      | user always gets a
//   (DEFAULT)            | PutItem AFTER the PATCH succeeds        | response; crash retries
//                        | (build_completion_record: condition="")| re-run; duplicate-success
//                        |                                        | skips
//   Claim (OPT-IN)       | CONDITIONAL PutItem                    | at-most-once execution;
//                        | attribute_not_exists(interaction_id)   | a crash AFTER the claim
//                        | BEFORE acting (build_claim_request)    | suppresses the retry =
//                        |                                        | possibly NO user response
//
// Use the completion marker by default (side effects are idempotent or a
// repeated PATCH is harmless). Reach for the claim ONLY when a side effect must
// never run twice (currency, purchases, irreversible external commands) and a
// dropped response is the lesser evil.
//
// The stored item is identical for both primitives; only the condition differs.
// DynamoDB item JSON uses attribute-type descriptors: "S" (string) for the
// snowflake interaction id, "N" (number-as-string) for the epoch TTL.

#include <cstdlib>
#include <string>

#include <nlohmann/json.hpp>

namespace discord_interactions {

using json = nlohmann::json;

// Table name + DynamoDB item + condition expression for a PutItem. An empty
// condition means an unconditional put (the completion marker); a non-empty
// condition is the claim guard.
struct ClaimRequestParts {
    std::string table{};
    json item{};
    std::string condition{};
};

namespace detail {

// Shared item shape for both primitives:
//   {"interaction_id": {"S": id}, "expires_at": {"N": to_string(now + ttl)}}
// DynamoDB carries numbers as strings under the "N" descriptor, hence to_string.
inline json idempotency_item(const std::string& interaction_id, int64_t now_epoch_s,
                             int64_t ttl_s) {
    json item{};
    item["interaction_id"] = json{{"S", interaction_id}};
    item["expires_at"] = json{{"N", std::to_string(now_epoch_s + ttl_s)}};
    return item;
}

}  // namespace detail

// Claim primitive (OPT-IN): conditional put that only succeeds if no record
// exists yet. A retry after the claim hits ConditionalCheckFailedException.
inline ClaimRequestParts build_claim_request(const std::string& table,
                                             const std::string& interaction_id,
                                             int64_t now_epoch_s, int64_t ttl_s = 3600) {
    ClaimRequestParts parts{};
    parts.table = table;
    parts.item = detail::idempotency_item(interaction_id, now_epoch_s, ttl_s);
    parts.condition = "attribute_not_exists(interaction_id)";
    return parts;
}

// Completion marker (DEFAULT): unconditional put, recorded ONLY after the
// user-visible effect (the PATCH) has succeeded. Empty condition = overwrite.
inline ClaimRequestParts build_completion_record(const std::string& table,
                                                 const std::string& interaction_id,
                                                 int64_t now_epoch_s, int64_t ttl_s = 3600) {
    ClaimRequestParts parts{};
    parts.table = table;
    parts.item = detail::idempotency_item(interaction_id, now_epoch_s, ttl_s);
    parts.condition = "";
    return parts;
}

// Opt-in switch for the whole feature. Unset or empty -> "" -> both primitives
// no-op to "proceed" (see idempotency_store.hpp).
inline std::string idempotency_table_from_env() {
    const char* table = std::getenv("DISCORD_IDEMPOTENCY_TABLE");
    if (table == nullptr || *table == '\0') {
        return "";
    }
    return std::string{table};
}

}  // namespace discord_interactions
