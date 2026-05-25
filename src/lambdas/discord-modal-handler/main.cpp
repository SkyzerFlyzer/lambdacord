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

std::string modal_function_name(const std::string& custom_id) {
    if (custom_id.empty()) {
        throw validation_error("modal submit interaction is missing custom_id");
    }

    const auto delimiter = custom_id.find(':');
    const std::string prefix = delimiter == std::string::npos
        ? custom_id
        : custom_id.substr(0, delimiter);

    if (prefix.empty()) {
        throw validation_error("modal submit interaction custom_id prefix is empty");
    }

    return "discord-modal-" + prefix;
}

void invoke_async(const std::string& function_name, const json& payload) {
    Aws::Lambda::Model::InvokeRequest request{};
    request.SetFunctionName(function_name);
    request.SetInvocationType(Aws::Lambda::Model::InvocationType::Event);
    request.SetContentType("application/json");
    auto body = Aws::MakeShared<Aws::StringStream>("ModalInvoke");
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
        const std::string custom_id = interaction.at("data").value("custom_id", "");
        invoke_async(modal_function_name(custom_id), interaction);
        return invocation_response::success(R"({"ok":true})", "application/json");
    } catch (const validation_error& ex) {
        std::cerr << "modal handler validation failed: " << ex.what() << "\n";
        return invocation_response::failure(ex.what(), "application/json");
    } catch (const std::exception& ex) {
        std::cerr << "modal handler failed: " << ex.what() << "\n";
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
