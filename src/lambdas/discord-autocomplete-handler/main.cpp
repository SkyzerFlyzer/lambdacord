#include <aws/core/Aws.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/lambda/LambdaClient.h>
#include <aws/lambda-runtime/runtime.h>
#include <discord_interactions/interaction.hpp>
#include <discord_interactions/lambda_client.hpp>
#include <discord_interactions/response.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

using json = nlohmann::json;
using namespace aws::lambda_runtime;

namespace {

Aws::SDKOptions g_sdk_options{};
std::shared_ptr<Aws::Lambda::LambdaClient> g_lambda_client{};

inline constexpr long autocomplete_request_timeout_ms = 2500;

class validation_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

bool append_focused_option(const json& options, std::ostringstream& output) {
    if (!options.is_array()) {
        return false;
    }

    for (const auto& option : options) {
        const int type = option.value("type", 0);
        const std::string name = option.value("name", "");
        if (type == 1 || type == 2) {
            if (append_focused_option(option.value("options", json::array()), output)) {
                return true;
            }
            continue;
        }

        if (option.value("focused", false)) {
            output << name;
            return true;
        }
    }

    return false;
}

std::string autocomplete_function_name(const json& interaction) {
    const json& data = interaction.at("data");
    std::ostringstream base_name{};
    base_name << data.value("name", "");
    discord_interactions::append_selected_command_path(
        data.value("options", json::array()), base_name);

    if (base_name.str().empty()) {
        throw validation_error("autocomplete interaction is missing an application command name");
    }

    const std::string command_name = base_name.str();
    std::string key = "discord-autocomplete-";
    key.reserve(key.size() + command_name.size());
    key += discord_interactions::route_suffix_from_path(command_name);

    std::ostringstream focused{};
    if (!append_focused_option(data.value("options", json::array()), focused)) {
        throw validation_error("autocomplete interaction has no focused option");
    }

    key.push_back('-');
    key += focused.str();

    return key;
}

json empty_autocomplete_response() {
    return discord_interactions::interaction_response(
        discord_interactions::response_autocomplete_result,
        json{{"choices", json::array()}});
}

json invoke_autocomplete_with_deadline(const std::string& function_name, const json& interaction) {
    auto task = std::make_shared<std::packaged_task<json()>>(
        [function_name, interaction]() {
            return discord_interactions::invoke_sync(
                *g_lambda_client,
                function_name,
                interaction,
                autocomplete_request_timeout_ms,
                "AutocompleteInvoke");
        });
    std::future<json> result = task->get_future();
    std::thread([task]() { (*task)(); }).detach();

    if (result.wait_for(std::chrono::milliseconds(autocomplete_request_timeout_ms)) !=
        std::future_status::ready) {
        throw std::runtime_error("autocomplete worker timed out");
    }
    return result.get();
}

invocation_response handler(const invocation_request& request) {
    try {
        const json interaction = json::parse(request.payload);
        const json response = invoke_autocomplete_with_deadline(
            autocomplete_function_name(interaction), interaction);
        return invocation_response::success(response.dump(), "application/json");
    } catch (const validation_error& ex) {
        std::cerr << "autocomplete handler validation failed: " << ex.what() << "\n";
        return invocation_response::failure(ex.what(), "application/json");
    } catch (const std::exception& ex) {
        std::cerr << "autocomplete handler failed: " << ex.what() << "\n";
        return invocation_response::success(
            empty_autocomplete_response().dump(), "application/json");
    }
}

}  // namespace

int main() {
    Aws::InitAPI(g_sdk_options);
    Aws::Client::ClientConfiguration config{};
    discord_interactions::configure_lambda_client(config, autocomplete_request_timeout_ms);
    g_lambda_client = std::make_shared<Aws::Lambda::LambdaClient>(config);
    run_handler(handler);
    g_lambda_client.reset();
    Aws::ShutdownAPI(g_sdk_options);
    return 0;
}
