#include <aws/core/Aws.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/dynamodb/DynamoDBClient.h>
#include <aws/dynamodb/DynamoDBErrors.h>
#include <aws/dynamodb/model/AttributeValue.h>
#include <aws/dynamodb/model/GetItemRequest.h>
#include <aws/dynamodb/model/PutItemRequest.h>
#include <aws/lambda/LambdaClient.h>
#include <aws/lambda-runtime/runtime.h>
#include <discord_interactions/interaction.hpp>
#include <discord_interactions/lambda_client.hpp>
#include <discord_interactions/response.hpp>
#include <nlohmann/json.hpp>
#include <sodium.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

using json = nlohmann::json;
using namespace aws::lambda_runtime;

namespace {

Aws::SDKOptions g_sdk_options{};
std::shared_ptr<Aws::Lambda::LambdaClient> g_lambda_client{};
std::shared_ptr<Aws::DynamoDB::DynamoDBClient> g_dynamo_client{};

bool verify_signature(const std::string& public_key_hex,
                      const std::string& signature_hex,
                      const std::string& timestamp,
                      const std::string& body) {
    if (public_key_hex.size() != 64 || signature_hex.size() != 128) {
        return false;
    }

    std::array<unsigned char, crypto_sign_PUBLICKEYBYTES> public_key{};
    std::array<unsigned char, crypto_sign_BYTES> signature{};

    if (sodium_hex2bin(public_key.data(), public_key.size(), public_key_hex.c_str(),
                       public_key_hex.size(), nullptr, nullptr, nullptr) != 0) {
        return false;
    }

    if (sodium_hex2bin(signature.data(), signature.size(), signature_hex.c_str(),
                       signature_hex.size(), nullptr, nullptr, nullptr) != 0) {
        return false;
    }

    const std::string signed_message = timestamp + body;
    return crypto_sign_verify_detached(
               signature.data(),
               reinterpret_cast<const unsigned char*>(signed_message.c_str()),
               signed_message.size(),
               public_key.data()) == 0;
}

bool should_skip_signature_verification() {
#ifdef DISCORD_ALLOW_SIGNATURE_BYPASS
    const char* value = std::getenv("DISCORD_SKIP_SIGNATURE_VERIFY");
    const bool skip = value != nullptr && value[0] == '1' && value[1] == '\0';
    if (skip) {
        std::cerr << "WARNING: signature verification bypassed\n";
    }
    return skip;
#else
    return false;
#endif
}

long max_signature_skew_seconds() {
    const char* value = std::getenv("DISCORD_SIGNATURE_MAX_SKEW_SECONDS");
    if (value == nullptr || *value == '\0') {
        return 60;
    }

    errno = 0;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || errno != 0 || parsed < 0) {
        std::cerr << "invalid DISCORD_SIGNATURE_MAX_SKEW_SECONDS; using default\n";
        return 60;
    }
    return parsed;
}

bool timestamp_is_fresh(const std::string& timestamp, long max_skew_seconds) {
    errno = 0;
    char* end = nullptr;
    const long long parsed = std::strtoll(timestamp.c_str(), &end, 10);
    if (end == timestamp.c_str() || *end != '\0' || errno != 0) {
        return false;
    }

    const long long now = static_cast<long long>(std::time(nullptr));
    return std::llabs(now - parsed) <= max_skew_seconds;
}

std::string request_body(const json& event) {
    const std::string body = event.value("body", "");
    if (!event.value("isBase64Encoded", false)) {
        return body;
    }

    std::vector<unsigned char> decoded(body.size());
    size_t decoded_len = 0;
    const int result = sodium_base642bin(
        decoded.data(),
        decoded.size(),
        body.c_str(),
        body.size(),
        nullptr,
        &decoded_len,
        nullptr,
        sodium_base64_VARIANT_ORIGINAL);
    if (result != 0) {
        throw std::runtime_error("request body base64 decode failed");
    }
    return std::string(reinterpret_cast<const char*>(decoded.data()), decoded_len);
}

std::string optional_env(const char* key) {
    const char* value = std::getenv(key);
    return value == nullptr ? "" : value;
}

long long dedup_expires_at() {
    return static_cast<long long>(std::time(nullptr)) + 900;
}

json dedup_error_response() {
    const std::string message = "Something went wrong, please try again.";
    return discord_interactions::interaction_response(
        discord_interactions::response_channel_message,
        json{{"flags", discord_interactions::ephemeral_flag},
             {"embeds", json::array({{{"description", message}, {"color", 0xE74C3C}}})}});
}

std::optional<std::string> read_dedup_status(const std::string& table,
                                             const std::string& interaction_id) {
    Aws::DynamoDB::Model::AttributeValue pk{};
    pk.SetS(interaction_id);

    Aws::DynamoDB::Model::GetItemRequest request{};
    request.SetTableName(table);
    request.AddKey("pk", pk);
    request.SetConsistentRead(true);

    const auto outcome = g_dynamo_client->GetItem(request);
    if (!outcome.IsSuccess()) {
        throw std::runtime_error("DynamoDB GetItem failed: " +
                                 outcome.GetError().GetMessage());
    }

    const auto& item = outcome.GetResult().GetItem();
    const auto status = item.find("status");
    if (status == item.end()) {
        return std::nullopt;
    }
    return status->second.GetS();
}

bool put_dedup_record(const std::string& table, const std::string& interaction_id) {
    Aws::DynamoDB::Model::AttributeValue pk{};
    pk.SetS(interaction_id);

    Aws::DynamoDB::Model::AttributeValue status{};
    status.SetS("processing");

    Aws::DynamoDB::Model::AttributeValue expires_at{};
    expires_at.SetN(std::to_string(dedup_expires_at()));

    Aws::DynamoDB::Model::PutItemRequest request{};
    request.SetTableName(table);
    request.AddItem("pk", pk);
    request.AddItem("status", status);
    request.AddItem("expires_at", expires_at);
    request.SetConditionExpression("attribute_not_exists(pk)");

    const auto outcome = g_dynamo_client->PutItem(request);
    if (outcome.IsSuccess()) {
        return true;
    }
    if (outcome.GetError().GetErrorType() ==
        Aws::DynamoDB::DynamoDBErrors::CONDITIONAL_CHECK_FAILED) {
        return false;
    }
    throw std::runtime_error("DynamoDB PutItem failed: " + outcome.GetError().GetMessage());
}

json handle_async_interaction(const json& interaction,
                              const std::string& function_name,
                              int deferred_response_type) {
    const json deferred_response =
        discord_interactions::interaction_response(deferred_response_type);
    const std::string table = optional_env("DISCORD_INTERACTION_DEDUP_TABLE");
    const std::string interaction_id = interaction.value("id", "");
    if (table.empty() || interaction_id.empty()) {
        discord_interactions::invoke_async(
            *g_lambda_client, function_name, interaction, "DiscordGatewayInvoke");
        return deferred_response;
    }

    try {
        if (put_dedup_record(table, interaction_id)) {
            discord_interactions::invoke_async(
                *g_lambda_client, function_name, interaction, "DiscordGatewayInvoke");
            return deferred_response;
        }

        const std::optional<std::string> status = read_dedup_status(table, interaction_id);
        if (status.has_value() && *status == "failed") {
            return dedup_error_response();
        }
        return deferred_response;
    } catch (const std::exception& ex) {
        std::cerr << "Discord interaction dedup failed open: " << ex.what() << "\n";
        discord_interactions::invoke_async(
            *g_lambda_client, function_name, interaction, "DiscordGatewayInvoke");
        return deferred_response;
    }
}

invocation_response handler(const invocation_request& request) {
    try {
        const json event = json::parse(request.payload);
        const json headers = event.value("headers", json::object());
        const std::string body = request_body(event);
        const std::string signature =
            discord_interactions::header_value(headers, "x-signature-ed25519");
        const std::string timestamp =
            discord_interactions::header_value(headers, "x-signature-timestamp");
        const std::string public_key = discord_interactions::require_env("DISCORD_PUBLIC_KEY");

        if (!should_skip_signature_verification()) {
            if (!verify_signature(public_key, signature, timestamp, body)) {
                std::cerr << "invalid Discord request signature\n";
                return discord_interactions::proxy_response(
                    401, json{{"error", "invalid request signature"}});
            }
            if (!timestamp_is_fresh(timestamp, max_signature_skew_seconds())) {
                std::cerr << "stale Discord request signature timestamp\n";
                return discord_interactions::proxy_response(
                    401, json{{"error", "invalid request signature"}});
            }
        }

        const json interaction = json::parse(body);
        const int type = interaction.value("type", 0);

        switch (type) {
        case 1:
            return discord_interactions::proxy_response(
                200,
                discord_interactions::interaction_response(discord_interactions::response_pong));
        case 2:
            return discord_interactions::proxy_response(
                200,
                handle_async_interaction(
                    interaction,
                    "discord-application-command-handler",
                    discord_interactions::response_deferred_channel_message));
        case 3:
            return discord_interactions::proxy_response(
                200,
                handle_async_interaction(
                    interaction,
                    "discord-message-component-handler",
                    discord_interactions::response_deferred_update_message));
        case 4:
            return discord_interactions::proxy_response(
                200,
                discord_interactions::invoke_sync(
                    *g_lambda_client,
                    "discord-autocomplete-handler",
                    interaction,
                    2500,
                    "DiscordGatewaySyncInvoke"));
        case 5:
            return discord_interactions::proxy_response(
                200,
                handle_async_interaction(
                    interaction,
                    "discord-modal-handler",
                    discord_interactions::response_deferred_channel_message));
        default:
            return discord_interactions::proxy_response(
                400, json{{"error", "unsupported interaction type"}});
        }
    } catch (const std::exception& ex) {
        std::cerr << "discord interaction ingress failed: " << ex.what() << "\n";
        return discord_interactions::proxy_response(500, json{{"error", "internal error"}});
    }
}

}  // namespace

int main() {
    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialization failed");
    }

    Aws::InitAPI(g_sdk_options);
    Aws::Client::ClientConfiguration config{};
    discord_interactions::configure_lambda_client(config);
    g_lambda_client = std::make_shared<Aws::Lambda::LambdaClient>(config);
    g_dynamo_client = std::make_shared<Aws::DynamoDB::DynamoDBClient>(config);

    run_handler(handler);
    g_dynamo_client.reset();
    g_lambda_client.reset();
    Aws::ShutdownAPI(g_sdk_options);
    return 0;
}
