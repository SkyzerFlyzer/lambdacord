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

// Look up `key` in the JSON-object route map held in env var `env_name`.
// Returns the mapped function name, or "" when the env var is unset/empty or
// has no string entry for the key. A malformed env value (invalid JSON or a
// non-object) must not throw — that would turn EVERY interaction into a
// generic failure — so it logs one stderr warning and falls back to
// mechanical derivation.
std::string route_map_override(const char* env_name, const std::string& key) {
    const char* route_map_env = std::getenv(env_name);
    const std::string route_map_json = route_map_env == nullptr ? "" : route_map_env;
    if (!route_map_json.empty()) {
        const json route_map =
            json::parse(route_map_json, nullptr, /*allow_exceptions=*/false);
        if (!route_map.is_object()) {
            std::cerr << env_name
                      << " is not a JSON object; falling back to mechanical route "
                         "derivation\n";
            return "";
        }
        const auto match = route_map.find(key);
        if (match != route_map.end() && match->is_string()) {
            return match->get<std::string>();
        }
    }
    return "";
}

std::string function_name_for_command(const std::string& command_name) {
    if (command_name.empty()) {
        throw std::runtime_error("application command interaction is missing a selected name");
    }

    const std::string mapped = route_map_override("DISCORD_COMMAND_ROUTES", command_name);
    if (!mapped.empty()) {
        return mapped;
    }

    std::string function_name = "discord-cmd-";
    function_name.reserve(function_name.size() + command_name.size());
    function_name += discord_interactions::route_suffix_from_path(command_name);
    return function_name;
}

// Context menu commands (data.type 2/3): consult the kind's env route map
// (keyed by the raw command name) first, mirroring DISCORD_COMMAND_ROUTES,
// then fall back to mechanical derivation: <prefix><lowercased name, spaces
// -> '-'> (e.g. "Report User" -> discord-usercmd-report-user).
std::string function_name_for_context_menu(
    const char* routes_env, const std::string& name_prefix, const std::string& command_name) {
    if (command_name.empty()) {
        throw std::runtime_error("context menu command interaction is missing a name");
    }

    const std::string mapped = route_map_override(routes_env, command_name);
    if (!mapped.empty()) {
        return mapped;
    }

    return name_prefix + discord_interactions::context_menu_route_suffix(command_name);
}

void invoke_async(const std::string& function_name, const json& payload) {
    Aws::Lambda::Model::InvokeRequest request{};
    request.SetFunctionName(function_name);
    request.SetInvocationType(Aws::Lambda::Model::InvocationType::Event);
    request.SetContentType("application/json");

    auto body = Aws::MakeShared<Aws::StringStream>("ApplicationCommandInvoke");
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
        const auto kind = discord_interactions::application_command_kind(interaction);
        if (kind == discord_interactions::ApplicationCommandKind::user) {
            const std::string command_name =
                interaction.value("data", json::object()).value("name", "");
            invoke_async(
                function_name_for_context_menu(
                    "DISCORD_USER_COMMAND_ROUTES", "discord-usercmd-", command_name),
                interaction);
        } else if (kind == discord_interactions::ApplicationCommandKind::message) {
            const std::string command_name =
                interaction.value("data", json::object()).value("name", "");
            invoke_async(
                function_name_for_context_menu(
                    "DISCORD_MESSAGE_COMMAND_ROUTES", "discord-msgcmd-", command_name),
                interaction);
        } else {
            const std::string command_name =
                discord_interactions::application_command_path(interaction);
            invoke_async(
                function_name_for_command(command_name),
                interaction);
        }
        return invocation_response::success(R"({"ok":true})", "application/json");
    } catch (const unknown_route_error& ex) {
        std::cerr << "application command handler unknown route: " << ex.what() << "\n";
        reply_unknown_route(interaction);
        return invocation_response::failure("unknown route", "application/json");
    } catch (const std::exception& ex) {
        std::cerr << "application command handler failed: " << ex.what() << "\n";
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
