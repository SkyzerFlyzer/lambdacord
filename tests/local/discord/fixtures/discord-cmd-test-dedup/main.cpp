// Integration-test fixture for task T5.3.
//
// This is NOT a deployed Lambda. It exists only so the `dedup` suite in
// tests/local/discord/run_local_tests.py can prove the durable
// completion-marker dedup (idempotency_store.hpp / T5.2, AD-9) end to end
// against the mock server's minimal DynamoDB surface, reached via the
// AWS_DYNAMODB_ENDPOINT override. The harness builds it on demand with
// scripts/build-lambda.sh (ensure_fixture_zip); it is never part of
// scripts/build-all-lambdas.sh and never packaged for deployment.
//
// Per AD-9 it runs the COMPLETION-MARKER flow (check -> act -> PATCH ->
// record), the framework default:
//   1. was_completed(id)  -> if a completion record exists, return
//      {"ok":true,"skipped":true} WITHOUT PATCHing (duplicate of a success).
//   2. Simulated crash    -> if the interaction JSON carries the TEST-ONLY
//      boolean field "test_fail_before_patch", log and fail the invocation
//      BEFORE the PATCH. The flag lives in the payload rather than an env var
//      so one warm container can serve both the crashing first delivery and
//      the clean retry — exactly the AWS async-retry shape the suite proves.
//      Real workers must never read such a field.
//   3. PATCH @original    -> the user-visible effect.
//   4. record_completion  -> ONLY after the PATCH succeeded, then {"ok":true}.
// A claim-at-start implementation records before crashing and suppresses the
// retry — the suite's crash-recovery case exists to fail such an
// implementation.

#include <aws/core/Aws.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/dynamodb/DynamoDBClient.h>
#include <aws/lambda-runtime/runtime.h>
#include <nlohmann/json.hpp>

#include <cstdlib>
#include <ctime>
#include <exception>
#include <iostream>
#include <memory>
#include <string>

#include "discord_interactions/idempotency_store.hpp"
#include "discord_interactions/interaction.hpp"
#include "discord_interactions/response.hpp"

using json = nlohmann::json;
using namespace aws::lambda_runtime;

namespace {

Aws::SDKOptions g_sdk_options{};
std::shared_ptr<Aws::DynamoDB::DynamoDBClient> g_dynamodb_client{};

// Mirrors lambda_client.hpp's configure_lambda_client, but honors the
// AWS_DYNAMODB_ENDPOINT override (the Phase 5 local-testing pattern) instead
// of AWS_LAMBDA_ENDPOINT.
void configure_dynamodb_client(Aws::Client::ClientConfiguration& config) {
    const char* region = std::getenv("AWS_REGION");
    config.region = region == nullptr ? "us-east-1" : region;
    config.caFile = "/etc/pki/tls/certs/ca-bundle.crt";
    config.enableTcpKeepAlive = true;
    config.connectTimeoutMs = 3000;
    config.requestTimeoutMs = 10000;

    const char* endpoint = std::getenv("AWS_DYNAMODB_ENDPOINT");
    if (endpoint != nullptr && *endpoint != '\0') {
        config.endpointOverride = endpoint;
        config.scheme = Aws::Http::Scheme::HTTP;
        config.verifySSL = false;
    }
}

invocation_response handler(const invocation_request& request) {
    try {
        const json interaction = json::parse(request.payload);
        const discord_interactions::InteractionMetadata meta =
            discord_interactions::metadata(interaction);
        const std::string interaction_id = interaction.value("id", "");
        if (meta.application_id.empty() || meta.token.empty() ||
            interaction_id.empty()) {
            std::cerr << "test-dedup: interaction is missing id, application_id,"
                         " or token"
                      << std::endl;
            return invocation_response::failure("internal error", "application/json");
        }

        const std::string table =
            discord_interactions::idempotency_table_from_env();

        // 1. Completion-marker check: skip work that already PATCHed, and say
        // so in the runtime response so the suite can tell the paths apart.
        if (discord_interactions::was_completed(*g_dynamodb_client, table,
                                                interaction_id)) {
            return invocation_response::success(R"({"ok":true,"skipped":true})",
                                                "application/json");
        }

        // 2. TEST-ONLY simulated crash BEFORE the PATCH (see header comment):
        // nothing was recorded, so AWS's async retry must re-run and PATCH.
        if (interaction.value("test_fail_before_patch", false)) {
            std::cerr << "test-dedup: simulated crash before PATCH"
                         " (test_fail_before_patch set)"
                      << std::endl;
            return invocation_response::failure("simulated crash before PATCH",
                                                "application/json");
        }

        // 3. The user-visible effect.
        const json patch_payload = {{"content", "test-dedup original response"}};
        discord_interactions::patch_original_response(meta.application_id,
                                                      meta.token, patch_payload);

        // 4. Record completion ONLY after the PATCH succeeded (AD-9).
        discord_interactions::record_completion(*g_dynamodb_client, table,
                                                interaction_id,
                                                std::time(nullptr));

        return invocation_response::success(R"({"ok":true})", "application/json");
    } catch (const std::exception& ex) {
        // Log internals to stderr only — never surface exception text to
        // Discord-facing payloads (CLAUDE.md error-copy rule).
        std::cerr << "test-dedup fixture failed: " << ex.what() << std::endl;
        return invocation_response::failure("internal error", "application/json");
    }
}

}  // namespace

int main() {
    Aws::InitAPI(g_sdk_options);
    // Warm-global client, constructed once in main() — the same pattern as the
    // routers' LambdaClient (idempotency_store.hpp's contract).
    Aws::Client::ClientConfiguration config{};
    configure_dynamodb_client(config);
    g_dynamodb_client = std::make_shared<Aws::DynamoDB::DynamoDBClient>(config);
    run_handler(handler);
    g_dynamodb_client.reset();
    Aws::ShutdownAPI(g_sdk_options);
    return 0;
}
