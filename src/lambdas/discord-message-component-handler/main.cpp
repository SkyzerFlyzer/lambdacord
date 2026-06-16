#include <aws/core/Aws.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/lambda/LambdaClient.h>
#include <aws/lambda-runtime/runtime.h>
#include <discord_interactions/interaction.hpp>
#include <discord_interactions/lambda_client.hpp>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

using json = nlohmann::json;
using namespace aws::lambda_runtime;

namespace {

Aws::SDKOptions g_sdk_options{};
std::shared_ptr<Aws::Lambda::LambdaClient> g_lambda_client{};

std::string component_function_name(const std::string& custom_id) {
    if (custom_id.empty()) {
        throw std::runtime_error("message component interaction is missing custom_id");
    }

    const std::string prefix = discord_interactions::component_prefix(custom_id);
    const char* route_map_env = std::getenv("DISCORD_COMPONENT_ROUTES");
    const std::string route_map_json = route_map_env == nullptr ? "" : route_map_env;
    if (!route_map_json.empty()) {
        const json route_map = json::parse(route_map_json);
        const auto match = route_map.find(prefix);
        if (match != route_map.end() && match->is_string()) {
            return match->get<std::string>();
        }
    }

    if (!discord_interactions::is_valid_route_prefix(prefix)) {
        throw std::runtime_error("message component interaction custom_id prefix is invalid");
    }
    return "discord-component-" + prefix;
}

invocation_response handler(const invocation_request& request) {
    try {
        const json interaction = json::parse(request.payload);
        const std::string custom_id = interaction.at("data").value("custom_id", "");
        discord_interactions::invoke_async(
            *g_lambda_client,
            component_function_name(custom_id),
            interaction,
            "ComponentInvoke");
        return invocation_response::success(R"({"ok":true})", "application/json");
    } catch (const std::exception& ex) {
        std::cerr << "message component handler failed: " << ex.what() << "\n";
        return invocation_response::failure("internal error", "application/json");
    }
}

}  // namespace

int main() {
    Aws::InitAPI(g_sdk_options);
    Aws::Client::ClientConfiguration config{};
    discord_interactions::configure_lambda_client(config);
    g_lambda_client = std::make_shared<Aws::Lambda::LambdaClient>(config);
    run_handler(handler);
    g_lambda_client.reset();
    Aws::ShutdownAPI(g_sdk_options);
    return 0;
}
