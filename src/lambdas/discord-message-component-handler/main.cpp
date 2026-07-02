#include <aws/core/Aws.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/lambda/LambdaClient.h>
#include <aws/lambda/model/InvocationType.h>
#include <aws/lambda/model/InvokeRequest.h>
#include <aws/lambda-runtime/runtime.h>
#include <discord_interactions/interaction.hpp>
#include <discord_interactions/lambda_client.hpp>
#include <discord_interactions/rest.hpp>
#include <discord_interactions/unknown_route.hpp>
#include <nlohmann/json.hpp>

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

// Thrown when routing resolves a worker Lambda that does not exist
// (ResourceNotFoundException). Handled specially: friendly PATCH, then still a
// failed invocation for observability.
class unknown_route_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// PATCH @original with the shared friendly copy after an interaction routed to
// a nonexistent worker Lambda. no_retry — routers are on the latency-sensitive
// path (AD-8). Any failure here is logged, never rethrown: the caller still
// fails the invocation. Never leaks the internal SDK error to the user.
// No "flags" field: Discord ignores flags on webhook message edits, so the
// reply simply keeps the visibility of the original deferred ACK.
void reply_unknown_route(const json& interaction) {
    try {
        const auto meta = discord_interactions::metadata(interaction);
        discord_interactions::discord_request(
            "PATCH",
            discord_interactions::webhook_url(meta.application_id, meta.token,
                                              "/messages/@original"),
            json{{"content", discord_interactions::unknown_route_user_copy}},
            discord_interactions::no_retry);
    } catch (const std::exception& patch_ex) {
        std::cerr << "failed to PATCH friendly unknown-route reply: " << patch_ex.what()
                  << "\n";
    }
}

void configure_lambda_client(Aws::Client::ClientConfiguration& config) {
    const char* region = std::getenv("AWS_REGION");
    config.region = region == nullptr ? "us-east-1" : region;
    config.caFile = "/etc/pki/tls/certs/ca-bundle.crt";
    config.enableTcpKeepAlive = true;

    const char* endpoint = std::getenv("AWS_LAMBDA_ENDPOINT");
    if (endpoint != nullptr && *endpoint != '\0') {
        config.endpointOverride = endpoint;
        config.scheme = Aws::Http::Scheme::HTTP;
        config.verifySSL = false;
    }
}

std::string component_function_name(const std::string& custom_id) {
    if (custom_id.empty()) {
        throw std::runtime_error("message component interaction is missing custom_id");
    }

    const std::string prefix = discord_interactions::component_prefix(custom_id);
    const char* route_map_env = std::getenv("DISCORD_COMPONENT_ROUTES");
    const std::string route_map_json = route_map_env == nullptr ? "" : route_map_env;
    if (!route_map_json.empty()) {
        // A malformed env value (invalid JSON or a non-object) must not throw —
        // that would turn EVERY component interaction into a generic failure —
        // so log one stderr warning and fall back to mechanical derivation.
        const json route_map =
            json::parse(route_map_json, nullptr, /*allow_exceptions=*/false);
        if (!route_map.is_object()) {
            std::cerr << "DISCORD_COMPONENT_ROUTES is not a JSON object; falling back "
                         "to mechanical route derivation\n";
        } else {
            const auto match = route_map.find(prefix);
            if (match != route_map.end() && match->is_string()) {
                return match->get<std::string>();
            }
        }
    }

    return "discord-component-" + prefix;
}

void invoke_async(const std::string& function_name, const json& payload) {
    Aws::Lambda::Model::InvokeRequest request{};
    request.SetFunctionName(function_name);
    request.SetInvocationType(Aws::Lambda::Model::InvocationType::Event);
    request.SetContentType("application/json");
    auto body = Aws::MakeShared<Aws::StringStream>("ComponentInvoke");
    *body << payload.dump();
    request.SetBody(body);

    auto outcome = g_lambda_client->Invoke(request);
    if (!outcome.IsSuccess()) {
        const auto& error = outcome.GetError();
        if (discord_interactions::is_function_not_found(error)) {
            throw unknown_route_error("no worker Lambda for route " + function_name +
                                      ": " + error.GetMessage());
        }
        throw std::runtime_error(
            "failed to invoke " + function_name + ": " + error.GetMessage());
    }
}

invocation_response handler(const invocation_request& request) {
    json interaction{};
    try {
        interaction = json::parse(request.payload);
        const std::string custom_id = interaction.at("data").value("custom_id", "");
        invoke_async(component_function_name(custom_id), interaction);
        return invocation_response::success(R"({"ok":true})", "application/json");
    } catch (const unknown_route_error& ex) {
        std::cerr << "message component handler unknown route: " << ex.what() << "\n";
        reply_unknown_route(interaction);
        return invocation_response::failure("unknown route", "application/json");
    } catch (const std::exception& ex) {
        std::cerr << "message component handler failed: " << ex.what() << "\n";
        return invocation_response::failure("internal error", "application/json");
    }
}

}  // namespace

int main() {
    Aws::InitAPI(g_sdk_options);
    Aws::Client::ClientConfiguration config{};
    configure_lambda_client(config);
    g_lambda_client = std::make_shared<Aws::Lambda::LambdaClient>(config);
    run_handler(handler);
    g_lambda_client.reset();
    Aws::ShutdownAPI(g_sdk_options);
    return 0;
}
