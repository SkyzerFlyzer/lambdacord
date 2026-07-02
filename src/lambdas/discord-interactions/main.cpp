#include <aws/core/Aws.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/lambda/LambdaClient.h>
#include <aws/lambda/model/InvocationType.h>
#include <aws/lambda/model/InvokeRequest.h>
#include <aws/lambda-runtime/runtime.h>
#include <discord_interactions/interaction.hpp>
#include <nlohmann/json.hpp>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

using json = nlohmann::json;
using namespace aws::lambda_runtime;

namespace {

Aws::SDKOptions g_sdk_options{};
std::shared_ptr<Aws::Lambda::LambdaClient> g_lambda_client{};

std::string header_value(const json& headers, const std::string& key) {
    if (!headers.is_object()) {
        return "";
    }

    const auto exact = headers.find(key);
    if (exact != headers.end() && exact->is_string()) {
        return exact->get<std::string>();
    }

    std::string lowered_key = key;
    std::transform(lowered_key.begin(), lowered_key.end(), lowered_key.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    for (auto it = headers.begin(); it != headers.end(); ++it) {
        if (!it.value().is_string()) {
            continue;
        }

        std::string candidate = it.key();
        std::transform(candidate.begin(), candidate.end(), candidate.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (candidate == lowered_key) {
            return it.value().get<std::string>();
        }
    }

    return "";
}

bool verify_signature(const std::string& public_key_hex,
                      const std::string& signature_hex,
                      const std::string& timestamp,
                      const std::string& body) {
    if (public_key_hex.size() != 64 || signature_hex.size() != 128) {
        return false;
    }

    std::array<unsigned char, crypto_sign_PUBLICKEYBYTES> public_key{};
    std::array<unsigned char, crypto_sign_BYTES> signature{};

    // cppcheck's experimental bug hunting cannot prove that value-initialized
    // std::array storage and std::string::c_str() buffers are initialized at
    // these C-API boundaries; both are guaranteed by the language. Suppressed
    // inline so the pre-commit checks stay meaningful for real findings.
    // cppcheck-suppress [bughuntingUninit, unmatchedSuppression]
    if (sodium_hex2bin(public_key.data(), public_key.size(), public_key_hex.c_str(),
                       public_key_hex.size(), nullptr, nullptr, nullptr) != 0) {
        return false;
    }

    // cppcheck-suppress [bughuntingUninit, unmatchedSuppression]
    if (sodium_hex2bin(signature.data(), signature.size(), signature_hex.c_str(),
                       signature_hex.size(), nullptr, nullptr, nullptr) != 0) {
        return false;
    }

    const std::string signed_message = timestamp + body;
    return crypto_sign_verify_detached(
               // cppcheck-suppress [bughuntingUninit, unmatchedSuppression]
               signature.data(),
               // cppcheck-suppress [bughuntingUninit, unmatchedSuppression]
               reinterpret_cast<const unsigned char*>(signed_message.c_str()),
               signed_message.size(),
               // cppcheck-suppress [bughuntingUninit, unmatchedSuppression]
               public_key.data()) == 0;
}

std::string require_env(const char* key) {
    const char* value = std::getenv(key);
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error(std::string("Missing required environment variable: ") + key);
    }
    return value;
}

bool should_skip_signature_verification() {
    const char* value = std::getenv("DISCORD_SKIP_SIGNATURE_VERIFY");
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

void configure_lambda_client(Aws::Client::ClientConfiguration& config) {
    config.region = require_env("AWS_REGION");
    config.caFile = "/etc/pki/tls/certs/ca-bundle.crt";
    config.enableTcpKeepAlive = true;
    config.connectTimeoutMs = 3000;
    config.requestTimeoutMs = 10000;

    const char* endpoint = std::getenv("AWS_LAMBDA_ENDPOINT");
    if (endpoint != nullptr && *endpoint != '\0') {
        config.endpointOverride = endpoint;
        config.scheme = Aws::Http::Scheme::HTTP;
        config.verifySSL = false;
    }
}

// Manifest-driven ephemeral defer (T3.2, AD-5). The generated Terraform emits
// the merged, comma-separated allowlist of command paths into the optional
// DISCORD_EPHEMERAL_DEFER_ROUTES env var; the ingress only parses that env var
// and never reads manifests at runtime. CHAT_INPUT commands match on the full
// derived command path ("account link"); context menu commands (data.type 2/3)
// apply the same allowlist check using the raw command name ("Report User") —
// the simplest rule consistent with exact-path matching.
bool ephemeral_defer_requested(const json& interaction) {
    const char* csv = std::getenv("DISCORD_EPHEMERAL_DEFER_ROUTES");
    if (csv == nullptr || *csv == '\0') {
        return false;
    }

    // A malformed interaction without an object `data` never opts in;
    // application_command_path requires the `data` key to exist.
    const auto data = interaction.find("data");
    if (data == interaction.end() || !data->is_object()) {
        return false;
    }

    std::string command_path{};
    const auto kind = discord_interactions::application_command_kind(interaction);
    if (kind == discord_interactions::ApplicationCommandKind::chat_input) {
        command_path = discord_interactions::application_command_path(interaction);
    } else {
        command_path = data->value("name", "");
    }

    // csv is guarded non-null and non-empty above; bug hunting cannot see
    // through the getenv contract (same class of false positive as in
    // verify_signature).
    // cppcheck-suppress [bughuntingUninit, unmatchedSuppression]
    return discord_interactions::route_in_csv_allowlist(command_path, csv);
}

invocation_response make_proxy_response(int status_code, const json& body) {
    const json response = {
        {"statusCode", status_code},
        {"headers", {{"Content-Type", "application/json"}}},
        {"body", body.dump()},
        {"isBase64Encoded", false},
    };
    return invocation_response::success(response.dump(), "application/json");
}

void invoke_async(const std::string& function_name, const json& payload) {
    Aws::Lambda::Model::InvokeRequest request{};
    request.SetFunctionName(function_name);
    request.SetInvocationType(Aws::Lambda::Model::InvocationType::Event);
    request.SetContentType("application/json");

    auto body = Aws::MakeShared<Aws::StringStream>("DiscordGatewayInvoke");
    *body << payload.dump();
    request.SetBody(body);

    const auto outcome = g_lambda_client->Invoke(request);
    if (!outcome.IsSuccess()) {
        throw std::runtime_error(
            "Failed to invoke " + function_name + ": " +
            outcome.GetError().GetMessage());
    }
}

json invoke_sync(const std::string& function_name, const json& payload) {
    Aws::Lambda::Model::InvokeRequest request{};
    request.SetFunctionName(function_name);
    request.SetInvocationType(Aws::Lambda::Model::InvocationType::RequestResponse);
    request.SetContentType("application/json");

    auto body = Aws::MakeShared<Aws::StringStream>("DiscordGatewaySyncInvoke");
    *body << payload.dump();
    request.SetBody(body);

    const auto outcome = g_lambda_client->Invoke(request);
    if (!outcome.IsSuccess()) {
        throw std::runtime_error(
            "Failed to invoke " + function_name + ": " +
            outcome.GetError().GetMessage());
    }
    const std::string function_error = outcome.GetResult().GetFunctionError();
    if (!function_error.empty()) {
        throw std::runtime_error(
            "Function " + function_name + " failed: " + function_error);
    }

    std::ostringstream response_payload{};
    response_payload << outcome.GetResult().GetPayload().rdbuf();
    return json::parse(response_payload.str());
}

invocation_response handler(const invocation_request& request) {
    try {
        const json event = json::parse(request.payload);
        const json headers = event.value("headers", json::object());
        const std::string body = event.value("body", "");
        const std::string signature = header_value(headers, "x-signature-ed25519");
        const std::string timestamp = header_value(headers, "x-signature-timestamp");
        const std::string public_key = require_env("DISCORD_PUBLIC_KEY");

        if (!should_skip_signature_verification() &&
            !verify_signature(public_key, signature, timestamp, body)) {
            return make_proxy_response(401, json{{"error", "invalid request signature"}});
        }

        const json interaction = json::parse(body);
        const int type = interaction.value("type", 0);

        switch (type) {
        case 1:
            return make_proxy_response(200, json{{"type", 1}});
        case 2: {
            invoke_async("discord-application-command-handler", interaction);
            json ack = json{{"type", 5}};
            if (ephemeral_defer_requested(interaction)) {
                ack["data"] = json{{"flags", 64}};
            }
            return make_proxy_response(200, ack);
        }
        case 3:
            invoke_async("discord-message-component-handler", interaction);
            return make_proxy_response(200, json{{"type", 6}});
        case 4:
            return make_proxy_response(
                200,
                invoke_sync("discord-autocomplete-handler", interaction));
        case 5:
            invoke_async("discord-modal-handler", interaction);
            return make_proxy_response(200, json{{"type", 5}});
        default:
            return make_proxy_response(400, json{{"error", "unsupported interaction type"}});
        }
    } catch (const std::exception& ex) {
        std::cerr << "discord interaction ingress failed: " << ex.what() << "\n";
        return make_proxy_response(500, json{{"error", "internal error"}});
    }
}

}  // namespace

int main() {
    if (sodium_init() < 0) {
        throw std::runtime_error("libsodium initialization failed");
    }

    Aws::InitAPI(g_sdk_options);
    Aws::Client::ClientConfiguration config{};
    configure_lambda_client(config);
    g_lambda_client = std::make_shared<Aws::Lambda::LambdaClient>(config);

    run_handler(handler);
    g_lambda_client.reset();
    Aws::ShutdownAPI(g_sdk_options);
    return 0;
}
