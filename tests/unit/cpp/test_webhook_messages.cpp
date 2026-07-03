#include <doctest/doctest.h>

#include <string>

// T2.2 unit tests. Per the structural constraint (same as T2.1): the unit
// CMake carries no curl/AWS include paths, so only the PURE path helpers live
// in rest_policy.hpp and are tested here. The curl-executing lifecycle
// functions live in webhook_messages.hpp (includes rest.hpp) and are covered
// by the integration harness (T2.3), never compiled into this suite.
#include "discord_interactions/rest_policy.hpp"

using discord_interactions::followup_message_path;
using discord_interactions::join_url;
using discord_interactions::original_message_path;
using discord_interactions::webhook_base_path;

// --- webhook_base_path ------------------------------------------------------

TEST_CASE("webhook_base_path builds /webhooks/APP/TOKEN for normal ids") {
    CHECK(webhook_base_path("123456789012345678", "tok_abc") ==
          "/webhooks/123456789012345678/tok_abc");
}

TEST_CASE("webhook_base_path embeds both ids in order (application id before token)") {
    const std::string path = webhook_base_path("APP", "TOKEN");
    const std::string::size_type app_pos = path.find("APP");
    const std::string::size_type token_pos = path.find("TOKEN");
    REQUIRE(app_pos != std::string::npos);
    REQUIRE(token_pos != std::string::npos);
    CHECK(app_pos < token_pos);
    CHECK(path == "/webhooks/APP/TOKEN");
}

// --- original_message_path --------------------------------------------------

TEST_CASE("original_message_path appends /messages/@original to the webhook base") {
    CHECK(original_message_path("APP", "TOKEN") ==
          "/webhooks/APP/TOKEN/messages/@original");
}

// --- followup_message_path --------------------------------------------------

TEST_CASE("followup_message_path targets a concrete message id") {
    CHECK(followup_message_path("APP", "TOKEN", "999888777") ==
          "/webhooks/APP/TOKEN/messages/999888777");
}

TEST_CASE("followup_message_path with an empty message id keeps the trailing seam") {
    // Edge case: an empty message id yields the collection path with a trailing
    // slash rather than swallowing the seam — callers must supply a real id.
    CHECK(followup_message_path("APP", "TOKEN", "") ==
          "/webhooks/APP/TOKEN/messages/");
}

// --- composition with join_url (webhook_url conventions) --------------------

TEST_CASE("webhook_base_path composes with a base URL under one-slash join_url seams") {
    CHECK(join_url("https://discord.com/api/v10", webhook_base_path("APP", "TOKEN")) ==
          "https://discord.com/api/v10/webhooks/APP/TOKEN");
}

TEST_CASE("original_message_path composes with a trailing-slash base to exactly one seam") {
    CHECK(join_url("https://discord.com/api/v10/",
                   original_message_path("APP", "TOKEN")) ==
          "https://discord.com/api/v10/webhooks/APP/TOKEN/messages/@original");
}

TEST_CASE("followup_message_path composes with a base URL under one-slash join_url seams") {
    CHECK(join_url("http://localhost:19001/api/v10",
                   followup_message_path("APP", "TOKEN", "42")) ==
          "http://localhost:19001/api/v10/webhooks/APP/TOKEN/messages/42");
}
