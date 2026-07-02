#pragma once

#include <aws/lambda-runtime/runtime.h>
#include <nlohmann/json.hpp>

#include <curl/curl.h>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>

#include "discord_interactions/components.hpp"

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

inline size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = reinterpret_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

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

inline void patch_original_response(const std::string& application_id,
                                    const std::string& interaction_token,
                                    const json& message_payload) {
    using curl_handle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
    using curl_header_list = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;

    curl_handle curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) {
        throw std::runtime_error("curl_easy_init failed");
    }

    std::string response{};
    const std::string base_url = discord_api_base_url();
    const std::string url = base_url + "/webhooks/" + application_id + "/" +
                            interaction_token + "/messages/@original";
    const std::string body = message_payload.dump();

    curl_header_list headers(curl_slist_append(nullptr, "Content-Type: application/json"),
                             &curl_slist_free_all);
    if (!headers) {
        throw std::runtime_error("curl_slist_append failed");
    }

    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, "PATCH");
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl.get(), CURLOPT_CAINFO, "/etc/pki/tls/certs/ca-bundle.crt");

    const CURLcode res = curl_easy_perform(curl.get());
    if (res != CURLE_OK) {
        throw std::runtime_error(std::string("Discord PATCH failed: ") + curl_easy_strerror(res));
    }

    long http_code = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code < 200 || http_code >= 300) {
        throw std::runtime_error(
            "Discord PATCH failed with HTTP " + std::to_string(http_code) + ": " + response);
    }
}

}  // namespace discord_interactions
