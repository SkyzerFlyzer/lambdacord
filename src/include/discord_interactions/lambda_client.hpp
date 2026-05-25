#pragma once

#include <aws/core/client/ClientConfiguration.h>
#include <aws/lambda/LambdaClient.h>
#include <aws/lambda/model/InvocationType.h>
#include <aws/lambda/model/InvokeRequest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <string>

namespace discord_interactions {

using json = nlohmann::json;

inline void configure_lambda_client(Aws::Client::ClientConfiguration& config) {
    const char* region = std::getenv("AWS_REGION");
    config.region = region == nullptr ? "us-east-1" : region;
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

inline void invoke_async(Aws::Lambda::LambdaClient& client,
                         const std::string& function_name,
                         const json& payload,
                         const char* allocation_tag = "DiscordInteractionsInvoke") {
    Aws::Lambda::Model::InvokeRequest request{};
    request.SetFunctionName(function_name);
    request.SetInvocationType(Aws::Lambda::Model::InvocationType::Event);
    request.SetContentType("application/json");

    auto body = Aws::MakeShared<Aws::StringStream>(allocation_tag);
    *body << payload.dump();
    request.SetBody(body);

    const auto outcome = client.Invoke(request);
    if (!outcome.IsSuccess()) {
        throw std::runtime_error("failed to invoke " + function_name + ": " +
                                 outcome.GetError().GetMessage());
    }
}

inline json invoke_sync(Aws::Lambda::LambdaClient& client,
                        const std::string& function_name,
                        const json& payload,
                        const char* allocation_tag = "DiscordInteractionsSyncInvoke") {
    Aws::Lambda::Model::InvokeRequest request{};
    request.SetFunctionName(function_name);
    request.SetInvocationType(Aws::Lambda::Model::InvocationType::RequestResponse);
    request.SetContentType("application/json");

    auto body = Aws::MakeShared<Aws::StringStream>(allocation_tag);
    *body << payload.dump();
    request.SetBody(body);

    const auto outcome = client.Invoke(request);
    if (!outcome.IsSuccess()) {
        throw std::runtime_error("failed to invoke " + function_name + ": " +
                                 outcome.GetError().GetMessage());
    }

    std::ostringstream response_payload{};
    response_payload << outcome.GetResult().GetPayload().rdbuf();
    return json::parse(response_payload.str());
}

}  // namespace discord_interactions
