#include <aws/core/Aws.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/lambda/LambdaClient.h>
#include <aws/lambda/model/InvocationType.h>
#include <aws/lambda/model/InvokeRequest.h>
#include <aws/lambda-runtime/runtime.h>
#include <discord_interactions/interaction.hpp>
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

std::string function_name_for_command(const std::string& command_name) {
    if (command_name.empty()) {
        throw std::runtime_error("application command interaction is missing a selected name");
    }

    const char* route_map_env = std::getenv("DISCORD_COMMAND_ROUTES");
    const std::string route_map_json = route_map_env == nullptr ? "" : route_map_env;
    if (!route_map_json.empty()) {
        const json route_map = json::parse(route_map_json);
        const auto match = route_map.find(command_name);
        if (match != route_map.end() && match->is_string()) {
            return match->get<std::string>();
        }
    }

    std::string function_name = "discord-cmd-";
    function_name.reserve(function_name.size() + command_name.size());
    function_name += discord_interactions::route_suffix_from_path(command_name);
    return function_name;
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
        throw std::runtime_error(
            "failed to invoke " + function_name + ": " +
            outcome.GetError().GetMessage());
    }
}

invocation_response handler(const invocation_request& request) {
    try {
        const json interaction = json::parse(request.payload);
        const std::string command_name = discord_interactions::application_command_path(interaction);
        invoke_async(
            function_name_for_command(command_name),
            interaction);
        return invocation_response::success(R"({"ok":true})", "application/json");
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
