#pragma once

// Interaction-webhook message lifecycle (T2.2): create followup, fetch/edit/
// delete messages on the interaction webhook-token surface — the equivalent of
// discord.js's interaction.followUp() / editReply() / deleteReply() and
// discord.py's followup helpers.
//
// Built on discord_request + the webhook-token URL conventions from rest.hpp
// (T2.1). URL construction uses the pure path helpers in rest_policy.hpp
// (webhook_base_path / original_message_path / followup_message_path) composed
// with the resolved base via join_url(discord_api_base_url(), <path>), which is
// exactly what webhook_url() does — the pure helpers keep URL construction
// unit-testable without curl. Behavior is integration-tested in T2.3.
//
// Retry policy: these run on ASYNC worker Lambdas, so they default to the full
// RestRetryPolicy{} (429/5xx retry with an 8000ms budget) rather than no_retry.
// Per AD-8 a worker should clamp that budget to its invocation deadline by
// passing retry_policy_for_deadline(request.deadline) so a sleep-retry never
// outlives the function timeout. Sync interaction paths (ingress, autocomplete)
// must not use this header's retrying default — they PATCH @original with
// no_retry via response.hpp instead.
//
// TOKEN LIFETIME: interaction tokens expire after 15 MINUTES. Long-running work
// that outlives the token cannot reply at all — and, in this serverless model,
// cannot outlive the Lambda timeout anyway. Workflows longer than the worker
// timeout need their own delivery design (e.g. a durable store plus a separate
// notification channel); that is out of scope for this framework.
//
// Error handling: discord_request throws ModuleError(category=upstream) on
// non-2xx/transport failure, carrying response bodies only in internal
// debug/context fields — never surface them to Discord users (CLAUDE.md rule).

#include <nlohmann/json.hpp>

#include <string>

#include "discord_interactions/rest.hpp"
#include "discord_interactions/rest_policy.hpp"

namespace discord_interactions {

using json = nlohmann::json;

// POST /webhooks/APP/TOKEN — create a followup message. Returns the created
// message object (parsed from the response body; a non-object body, e.g. an
// empty response, parses to a null json).
inline json create_followup(const std::string& application_id, const std::string& token,
                            const json& message, const RestRetryPolicy& policy = {}) {
    const RestResponse response = discord_request(
        "POST", join_url(discord_api_base_url(), webhook_base_path(application_id, token)),
        message, policy);
    return json::parse(response.body, nullptr, /*allow_exceptions=*/false);
}

// GET /webhooks/APP/TOKEN/messages/@original — fetch the original interaction
// response message. Returns the parsed message object.
inline json get_original(const std::string& application_id, const std::string& token,
                         const RestRetryPolicy& policy = {}) {
    const RestResponse response = discord_request(
        "GET",
        join_url(discord_api_base_url(), original_message_path(application_id, token)),
        json(), policy);
    return json::parse(response.body, nullptr, /*allow_exceptions=*/false);
}

// PATCH /webhooks/APP/TOKEN/messages/MSG — edit a previously sent followup (or
// any message id owned by this interaction token).
inline void edit_followup(const std::string& application_id, const std::string& token,
                          const std::string& message_id, const json& message,
                          const RestRetryPolicy& policy = {}) {
    discord_request(
        "PATCH",
        join_url(discord_api_base_url(),
                 followup_message_path(application_id, token, message_id)),
        message, policy);
}

// DELETE /webhooks/APP/TOKEN/messages/@original — delete the original
// interaction response.
inline void delete_original(const std::string& application_id, const std::string& token,
                            const RestRetryPolicy& policy = {}) {
    discord_request(
        "DELETE",
        join_url(discord_api_base_url(), original_message_path(application_id, token)),
        json(), policy);
}

// DELETE /webhooks/APP/TOKEN/messages/MSG — delete a specific followup message.
inline void delete_followup(const std::string& application_id, const std::string& token,
                            const std::string& message_id,
                            const RestRetryPolicy& policy = {}) {
    discord_request(
        "DELETE",
        join_url(discord_api_base_url(),
                 followup_message_path(application_id, token, message_id)),
        json(), policy);
}

}  // namespace discord_interactions
