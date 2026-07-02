// Integration-test fixture for task T2.3.
//
// This is NOT a deployed Lambda. It exists only so the `rest` suite in
// tests/local/discord/run_local_tests.py can exercise the T2.1/T2.2 REST layer
// against the live mock Discord server (mock_lambda_server.py), reached via
// the DISCORD_API_BASE_URL override honored by rest.hpp. The harness builds it
// on demand with scripts/build-lambda.sh (ensure_fixture_zip); it is never
// part of scripts/build-all-lambdas.sh and never packaged for deployment.
//
// On each invocation it walks the interaction-webhook message lifecycle:
//   1. PATCH @original — deliberately via discord_request with the DEFAULT
//      RestRetryPolicy, NOT patch_original_response. patch_original_response's
//      no_retry contract (T2.1) exists for the sync gateway paths (ingress,
//      autocomplete), which must never sleep; this fixture is an async worker,
//      and per AD-8 async workers may sleep-retry, which is exactly the path
//      the suite proves by serving a 429-then-200 sequence from the mock.
//   2. POST a followup via create_followup (webhook_messages.hpp, T2.2).
//   3. DELETE that followup via delete_followup.
// Only after all three succeed does it return {"ok":true} to the runtime.

#include <aws/lambda-runtime/runtime.h>
#include <nlohmann/json.hpp>

#include <exception>
#include <iostream>
#include <string>

#include "discord_interactions/interaction.hpp"
#include "discord_interactions/rest.hpp"
#include "discord_interactions/rest_policy.hpp"
#include "discord_interactions/webhook_messages.hpp"

using json = nlohmann::json;
using namespace aws::lambda_runtime;

namespace {

invocation_response handler(const invocation_request& request) {
    try {
        const json interaction = json::parse(request.payload);
        const discord_interactions::InteractionMetadata meta =
            discord_interactions::metadata(interaction);
        if (meta.application_id.empty() || meta.token.empty()) {
            std::cerr << "test-echo: interaction is missing application_id or token"
                      << std::endl;
            return invocation_response::failure("internal error", "application/json");
        }

        // 1. PATCH @original with the DEFAULT retry policy (see header comment:
        // this must NOT be patch_original_response, whose no_retry contract is
        // for the sync gateway paths — the retry loop is the point here).
        const json patch_payload = {{"content", "test-echo original response"}};
        discord_interactions::discord_request(
            "PATCH",
            discord_interactions::webhook_url(meta.application_id, meta.token,
                                              "/messages/@original"),
            patch_payload, discord_interactions::RestRetryPolicy{});

        // 2. Followup (ephemeral per house style: all ephemeral responses use
        // flags 64).
        const json followup_payload = {{"content", "test-echo followup"},
                                       {"flags", 64}};
        const json followup = discord_interactions::create_followup(
            meta.application_id, meta.token, followup_payload);
        const std::string followup_id =
            followup.is_object() ? followup.value("id", "") : std::string{};
        if (followup_id.empty()) {
            std::cerr << "test-echo: followup response carried no message id"
                      << std::endl;
            return invocation_response::failure("internal error", "application/json");
        }

        // 3. Delete the followup we just created.
        discord_interactions::delete_followup(meta.application_id, meta.token,
                                              followup_id);

        return invocation_response::success(R"({"ok":true})", "application/json");
    } catch (const std::exception& ex) {
        // Log internals to stderr only — never surface exception text to
        // Discord-facing payloads (CLAUDE.md error-copy rule).
        std::cerr << "test-echo fixture failed: " << ex.what() << std::endl;
        return invocation_response::failure("internal error", "application/json");
    }
}

}  // namespace

int main() {
    run_handler(handler);
    return 0;
}
