#include <aws/core/Aws.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/lambda/LambdaClient.h>
#include <aws/lambda/model/InvocationType.h>
#include <aws/lambda/model/InvokeRequest.h>
#include <aws/lambda-runtime/runtime.h>
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

class validation_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

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

void append_selected_path(const json& options, std::ostringstream& full_name) {
    if (!options.is_array()) {
        return;
    }

    for (const auto& option : options) {
        const int type = option.value("type", 0);
        if (type != 1 && type != 2) {
            continue;
        }

        const std::string name = option.value("name", "");
        if (name.empty()) {
            return;
        }

        full_name << ' ' << name;
        append_selected_path(option.value("options", json::array()), full_name);
        return;
    }
}

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
    append_selected_path(data.value("options", json::array()), base_name);

    if (base_name.str().empty()) {
        throw validation_error("autocomplete interaction is missing an application command name");
    }

    const std::string command_name = base_name.str();
    std::string key = "discord-autocomplete-";
    key.reserve(key.size() + command_name.size());
    std::transform(command_name.begin(), command_name.end(), std::back_inserter(key),
                   [](char ch) { return ch == ' ' ? '-' : ch; });

    std::ostringstream focused{};
    if (!append_focused_option(data.value("options", json::array()), focused)) {
        throw validation_error("autocomplete interaction has no focused option");
    }

    key.push_back('-');
    key += focused.str();

    return key;
}

json invoke_sync(const std::string& function_name, const json& payload) {
    Aws::Lambda::Model::InvokeRequest request{};
    request.SetFunctionName(function_name);
    request.SetInvocationType(Aws::Lambda::Model::InvocationType::RequestResponse);
    request.SetContentType("application/json");
    auto body = Aws::MakeShared<Aws::StringStream>("AutocompleteInvoke");
    *body << payload.dump();
    request.SetBody(body);

    auto outcome = g_lambda_client->Invoke(request);

    if (!outcome.IsSuccess()) {
        throw std::runtime_error(
            "failed to invoke " + function_name + ": " +
            outcome.GetError().GetMessage());
    }

    std::ostringstream response_payload{};
    response_payload << outcome.GetResult().GetPayload().rdbuf();
    return json::parse(response_payload.str());
}

invocation_response handler(const invocation_request& request) {
    try {
        const json interaction = json::parse(request.payload);
        const json response =
            invoke_sync(autocomplete_function_name(interaction), interaction);
        return invocation_response::success(response.dump(), "application/json");
    } catch (const validation_error& ex) {
        std::cerr << "autocomplete handler validation failed: " << ex.what() << "\n";
        return invocation_response::failure(ex.what(), "application/json");
    } catch (const std::exception& ex) {
        std::cerr << "autocomplete handler failed: " << ex.what() << "\n";
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
