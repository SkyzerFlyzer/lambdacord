// T3.4 — Friendly unknown-route replies.
//
// The classification helper is_function_not_found() lives in lambda_client.hpp,
// which pulls in the AWS SDK Lambda client headers/types. The unit suite
// deliberately compiles WITHOUT AWS SDK, libsodium, or curl linkage (see
// tests/unit/cpp/CMakeLists.txt), so an Aws::Client::AWSError<LambdaErrors>
// cannot be constructed or included here. That helper is therefore covered by
// the integration harness (tests/local/discord/run_local_tests.py), which
// drives real routers against a mock returning Lambda's ResourceNotFoundException
// shape. This unit file owns the network-free half: the shared user-facing copy
// constant, which any framework header can include without AWS SDK.

#include <doctest/doctest.h>

#include <string>

#include "discord_interactions/unknown_route.hpp"

TEST_CASE("unknown_route_user_copy is friendly, non-empty, and placeholder-free") {
    const std::string copy = discord_interactions::unknown_route_user_copy;

    CHECK_FALSE(copy.empty());

    // Surfaced verbatim to Discord users, so it must carry no unfilled format
    // placeholders (printf-style or brace-style) that would leak literal
    // "%s"/"{}" tokens into the reply.
    CHECK(copy.find('%') == std::string::npos);
    CHECK(copy.find('{') == std::string::npos);
    CHECK(copy.find('}') == std::string::npos);

    // Must not leak internal/technical error vocabulary to users.
    CHECK(copy.find("ResourceNotFound") == std::string::npos);
    CHECK(copy.find("Function not found") == std::string::npos);
    CHECK(copy.find("Lambda") == std::string::npos);

    // Pin the exact copy so routers and tests share one source of truth.
    CHECK(copy == "That command isn't available right now.");
}
