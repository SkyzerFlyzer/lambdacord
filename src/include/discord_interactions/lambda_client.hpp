#pragma once

#include <aws/core/client/AWSError.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/lambda/LambdaClient.h>
#include <aws/lambda/LambdaErrors.h>
#include <aws/lambda/model/InvocationType.h>
#include <aws/lambda/model/InvokeRequest.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <string>

namespace discord_interactions {

using json = nlohmann::json;

// True when an Invoke outcome's error is Lambda's ResourceNotFoundException —
// i.e. the target function name does not exist. Routers use this to tell an
// unregistered route apart from any other invoke failure so they can surface a
// friendly "route unavailable" reply (T3.4) instead of a silent failure.
//
// Classification is by the SDK's mapped error enum: the Lambda control plane
// answers a missing function with HTTP 404 and header
// `x-amzn-ErrorType: ResourceNotFoundException`, which the SDK unmarshals to
// LambdaErrors::RESOURCE_NOT_FOUND. As a defensive fallback it also matches the
// exception name string, in case a gateway returns the shape without the enum
// mapping.
inline bool is_function_not_found(
    const Aws::Client::AWSError<Aws::Lambda::LambdaErrors>& error) {
    return error.GetErrorType() == Aws::Lambda::LambdaErrors::RESOURCE_NOT_FOUND ||
           error.GetExceptionName() == "ResourceNotFoundException";
}

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
