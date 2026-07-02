#pragma once

// Durable, cross-container interaction dedup backed by DynamoDB (AD-9 / Phase 5).
//
// This is the storage-backed counterpart to idempotency.hpp's in-process
// CompletedInteractions guard: the in-process guard only catches duplicates on
// the SAME warm container, while these primitives survive across containers and
// cold starts. The pure request-shaping lives in idempotency_requests.hpp; this
// header adds the three functions that actually execute against DynamoDB.
//
// AD-9 decision table (summary — full table in the Phase 5 plan section):
//
//   * Completion marker is the DEFAULT: check -> act -> PATCH -> record.
//     was_completed() does a GetItem BEFORE acting; record_completion() does an
//     UNCONDITIONAL PutItem AFTER the PATCH succeeded. A retry of a run that
//     CRASHED before the PATCH finds no record and correctly RE-RUNS, so the
//     user always gets a response. Duplicates of a success skip. The only
//     residual double-execution window is crash-after-PATCH, which merely
//     repeats an idempotent PATCH.
//
//   * Claim is OPT-IN for NON-IDEMPOTENT side effects: claim_interaction() does
//     a CONDITIONAL PutItem attribute_not_exists(interaction_id) BEFORE acting.
//     A retry after the claim hits ConditionalCheckFailedException and is
//     suppressed (returns false). This guarantees at-most-once execution, but a
//     crash AFTER the claim and BEFORE the PATCH means the retry is suppressed
//     and the user may get NO response — accept this only where a dropped
//     response is strictly better than running the side effect twice.
//
// Two universal behaviors, per the Phase 5 decision table:
//   * OPT-OUT: an empty `table` (env DISCORD_IDEMPOTENCY_TABLE unset) makes every
//     function a no-op that returns the "proceed" answer (was_completed -> false,
//     claim_interaction -> true, record_completion -> nothing). The feature is
//     off by default.
//   * FAIL-OPEN: any DynamoDB/infra error is logged to stderr and swallowed; the
//     function returns the "proceed" answer. A duplicate Discord PATCH is
//     strictly better than a silently dropped interaction. The ONLY non-proceed
//     result is a genuine ConditionalCheckFailedException on the claim path.
//
// Warm-global client: workers adopting these primitives must construct the
// Aws::DynamoDB::DynamoDBClient ONCE as a warm global in main() (same pattern as
// lambda_client.hpp's LambdaClient) and pass it by reference here — never build a
// client per invocation. Configure its endpoint from AWS_DYNAMODB_ENDPOINT for
// local testing, mirroring AWS_LAMBDA_ENDPOINT.

#include <iostream>
#include <string>

#include <aws/dynamodb/DynamoDBClient.h>
#include <aws/dynamodb/DynamoDBErrors.h>
#include <aws/dynamodb/model/AttributeValue.h>
#include <aws/dynamodb/model/GetItemRequest.h>
#include <aws/dynamodb/model/PutItemRequest.h>

#include "discord_interactions/idempotency_requests.hpp"

namespace discord_interactions {

namespace detail {

// Translate a pure ClaimRequestParts item (nlohmann JSON with S/N descriptors)
// into a DynamoDB PutItemRequest, applying the condition expression only when
// non-empty (empty => unconditional overwrite).
inline Aws::DynamoDB::Model::PutItemRequest to_put_item_request(const ClaimRequestParts& parts) {
    Aws::DynamoDB::Model::PutItemRequest request{};
    request.SetTableName(parts.table);

    for (auto it = parts.item.begin(); it != parts.item.end(); ++it) {
        const json& descriptor = it.value();
        Aws::DynamoDB::Model::AttributeValue value{};
        if (descriptor.contains("S")) {
            value.SetS(descriptor["S"].get<std::string>());
        } else if (descriptor.contains("N")) {
            value.SetN(descriptor["N"].get<std::string>());
        }
        request.AddItem(it.key(), value);
    }

    if (!parts.condition.empty()) {
        request.SetConditionExpression(parts.condition);
    }
    return request;
}

}  // namespace detail

// Completion-marker read (DEFAULT primitive). Returns true ONLY when a completion
// record exists. Empty table -> opt-out no-op -> false. Any infra error ->
// fail-open (logged) -> false, so the worker proceeds and re-runs.
inline bool was_completed(Aws::DynamoDB::DynamoDBClient& client, const std::string& table,
                          const std::string& interaction_id) {
    if (table.empty()) {
        return false;
    }

    Aws::DynamoDB::Model::GetItemRequest request{};
    request.SetTableName(table);
    Aws::DynamoDB::Model::AttributeValue key{};
    key.SetS(interaction_id);
    request.AddKey("interaction_id", key);
    request.SetConsistentRead(true);

    const auto outcome = client.GetItem(request);
    if (!outcome.IsSuccess()) {
        std::cerr << "idempotency: was_completed GetItem failed (fail-open, proceeding): "
                  << outcome.GetError().GetMessage() << std::endl;
        return false;
    }
    return !outcome.GetResult().GetItem().empty();
}

// Completion-marker write (DEFAULT primitive). Call ONLY AFTER the user-visible
// effect (the PATCH) succeeded. Empty table -> no-op. Any infra error is logged
// and swallowed: failing to record must never fail the invocation.
inline void record_completion(Aws::DynamoDB::DynamoDBClient& client, const std::string& table,
                              const std::string& interaction_id, int64_t now_epoch_s,
                              int64_t ttl_s = 3600) {
    if (table.empty()) {
        return;
    }

    const ClaimRequestParts parts = build_completion_record(table, interaction_id, now_epoch_s, ttl_s);
    const auto outcome = client.PutItem(detail::to_put_item_request(parts));
    if (!outcome.IsSuccess()) {
        std::cerr << "idempotency: record_completion PutItem failed (swallowed): "
                  << outcome.GetError().GetMessage() << std::endl;
    }
}

// Claim primitive (OPT-IN). Returns true = proceed (claimed, or table empty
// [opt-out], or infra error [fail-open, logged]); false = duplicate, and ONLY
// when the conditional put fails with ConditionalCheckFailedException.
inline bool claim_interaction(Aws::DynamoDB::DynamoDBClient& client, const std::string& table,
                              const std::string& interaction_id, int64_t now_epoch_s,
                              int64_t ttl_s = 3600) {
    if (table.empty()) {
        return true;
    }

    const ClaimRequestParts parts = build_claim_request(table, interaction_id, now_epoch_s, ttl_s);
    const auto outcome = client.PutItem(detail::to_put_item_request(parts));
    if (outcome.IsSuccess()) {
        return true;
    }

    if (outcome.GetError().GetErrorType() ==
        Aws::DynamoDB::DynamoDBErrors::CONDITIONAL_CHECK_FAILED) {
        // A prior run already claimed this interaction: suppress the duplicate.
        return false;
    }

    std::cerr << "idempotency: claim_interaction PutItem failed (fail-open, proceeding): "
              << outcome.GetError().GetMessage() << std::endl;
    return true;
}

}  // namespace discord_interactions
