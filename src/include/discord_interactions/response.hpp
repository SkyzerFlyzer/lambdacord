#pragma once

#include <aws/lambda-runtime/runtime.h>
#include <nlohmann/json.hpp>

#include <string>

#include "discord_interactions/components.hpp"
#include "discord_interactions/rest.hpp"

namespace discord_interactions {

using json = nlohmann::json;

inline constexpr int ephemeral_flag = 64;
inline constexpr int response_pong = 1;
inline constexpr int response_channel_message = 4;
inline constexpr int response_deferred_channel_message = 5;
inline constexpr int response_deferred_update_message = 6;
inline constexpr int response_update_message = 7;
inline constexpr int response_autocomplete_result = 8;
inline constexpr int response_modal = 9;
inline constexpr int response_premium_required = 10;  // DEPRECATED by Discord — use premium_button instead

// write_callback and discord_api_base_url moved to rest.hpp (T2.1) — still
// declared in namespace discord_interactions and re-exported via the include
// above, so existing callers of this header are unchanged.

inline aws::lambda_runtime::invocation_response proxy_response(int status_code,
                                                               const json& body) {
    const json response = {
        {"statusCode", status_code},
        {"headers", {{"Content-Type", "application/json"}}},
        {"body", body.dump()},
        {"isBase64Encoded", false},
    };
    return aws::lambda_runtime::invocation_response::success(response.dump(), "application/json");
}

inline json interaction_response(int type, const json& data = json::object()) {
    json response = {{"type", type}};
    if (!data.empty()) {
        response["data"] = data;
    }
    return response;
}

inline json ephemeral_message(const std::string& content) {
    return {{"content", content}, {"flags", ephemeral_flag}};
}

// A one-row action row holding a single link button. Refactored to delegate to
// components.hpp (button() + action_row()); the emitted JSON shape is unchanged
// so existing callers keep working byte-for-byte.
inline json link_button_row(const std::string& label, const std::string& url) {
    return json::array(
        {action_row(json::array({button(ButtonStyle::link, url, label)}))});
}

// Thin wrapper over discord_request + webhook_url (rest.hpp, T2.1).
// Contract unchanged: same signature, throws on any non-2xx outcome or
// transport failure. Deliberately passes no_retry to keep the original
// single-attempt, zero-sleep semantics — ingress/router callers sit on sync
// interaction paths where sleeping is forbidden (AD-8). Async workers that
// want rate-limit retries should call discord_request directly with
// retry_policy_for_deadline(request.deadline).
inline void patch_original_response(const std::string& application_id,
                                    const std::string& interaction_token,
                                    const json& message_payload) {
    discord_request("PATCH",
                    webhook_url(application_id, interaction_token, "/messages/@original"),
                    message_payload, no_retry);
}

}  // namespace discord_interactions
