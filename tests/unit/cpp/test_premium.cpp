#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "discord_interactions/premium.hpp"
// NOTE: response.hpp is deliberately NOT included here. It pulls in
// aws-lambda-runtime and curl headers that are intentionally absent from the
// unit-test include path, so the constant addition is verified by grep-asserting
// the header source (the alternative the T1.10 spec permits to static_assert).

#include <fstream>
#include <sstream>
#include <string>

using nlohmann::json;
using discord_interactions::entitlements;
using discord_interactions::has_entitlement_for_sku;
using discord_interactions::premium_button;

namespace {

json interaction_with(json ents) {
    return json{{"type", 2}, {"entitlements", std::move(ents)}};
}

}  // namespace

TEST_CASE("entitlements returns empty array when absent") {
    const json interaction = json{{"type", 2}};
    const json result = entitlements(interaction);
    CHECK(result.is_array());
    CHECK(result.empty());
}

TEST_CASE("entitlements returns empty array when not an array") {
    const json interaction = json{{"type", 2}, {"entitlements", "nope"}};
    const json result = entitlements(interaction);
    CHECK(result.is_array());
    CHECK(result.empty());
}

TEST_CASE("entitlements passes through the interaction array") {
    const json interaction = interaction_with(json::array({{{"sku_id", "abc"}}}));
    const json result = entitlements(interaction);
    REQUIRE(result.is_array());
    REQUIRE(result.size() == 1);
    CHECK(result[0]["sku_id"] == "abc");
}

TEST_CASE("has_entitlement_for_sku false when entitlements missing") {
    const json interaction = json{{"type", 2}};
    CHECK_FALSE(has_entitlement_for_sku(interaction, "sku_1"));
}

TEST_CASE("has_entitlement_for_sku false when entitlements empty") {
    const json interaction = interaction_with(json::array());
    CHECK_FALSE(has_entitlement_for_sku(interaction, "sku_1"));
}

TEST_CASE("has_entitlement_for_sku true for matching sku") {
    const json interaction = interaction_with(json::array({{{"sku_id", "sku_1"}}}));
    CHECK(has_entitlement_for_sku(interaction, "sku_1"));
}

TEST_CASE("has_entitlement_for_sku false for non-matching sku") {
    const json interaction = interaction_with(json::array({{{"sku_id", "sku_2"}}}));
    CHECK_FALSE(has_entitlement_for_sku(interaction, "sku_1"));
}

TEST_CASE("has_entitlement_for_sku excludes deleted entitlements") {
    const json interaction =
        interaction_with(json::array({{{"sku_id", "sku_1"}, {"deleted", true}}}));
    CHECK_FALSE(has_entitlement_for_sku(interaction, "sku_1"));
}

TEST_CASE("has_entitlement_for_sku deleted:false still matches") {
    const json interaction =
        interaction_with(json::array({{{"sku_id", "sku_1"}, {"deleted", false}}}));
    CHECK(has_entitlement_for_sku(interaction, "sku_1"));
}

TEST_CASE("has_entitlement_for_sku ends_at null passes") {
    const json interaction =
        interaction_with(json::array({{{"sku_id", "sku_1"}, {"ends_at", nullptr}}}));
    CHECK(has_entitlement_for_sku(interaction, "sku_1", "2026-07-02T00:00:00.000000+00:00"));
}

TEST_CASE("has_entitlement_for_sku absent ends_at passes with now supplied") {
    const json interaction = interaction_with(json::array({{{"sku_id", "sku_1"}}}));
    CHECK(has_entitlement_for_sku(interaction, "sku_1", "2026-07-02T00:00:00.000000+00:00"));
}

TEST_CASE("has_entitlement_for_sku past ends_at fails when now supplied") {
    const json interaction = interaction_with(
        json::array({{{"sku_id", "sku_1"}, {"ends_at", "2020-01-01T00:00:00.000000+00:00"}}}));
    CHECK_FALSE(has_entitlement_for_sku(interaction, "sku_1", "2026-07-02T00:00:00.000000+00:00"));
}

TEST_CASE("has_entitlement_for_sku future ends_at passes when now supplied") {
    const json interaction = interaction_with(
        json::array({{{"sku_id", "sku_1"}, {"ends_at", "2099-01-01T00:00:00.000000+00:00"}}}));
    CHECK(has_entitlement_for_sku(interaction, "sku_1", "2026-07-02T00:00:00.000000+00:00"));
}

TEST_CASE("has_entitlement_for_sku future starts_at fails when now supplied") {
    const json interaction = interaction_with(
        json::array({{{"sku_id", "sku_1"}, {"starts_at", "2099-01-01T00:00:00.000000+00:00"}}}));
    CHECK_FALSE(has_entitlement_for_sku(interaction, "sku_1", "2026-07-02T00:00:00.000000+00:00"));
}

TEST_CASE("has_entitlement_for_sku past starts_at passes when now supplied") {
    const json interaction = interaction_with(
        json::array({{{"sku_id", "sku_1"}, {"starts_at", "2020-01-01T00:00:00.000000+00:00"}}}));
    CHECK(has_entitlement_for_sku(interaction, "sku_1", "2026-07-02T00:00:00.000000+00:00"));
}

TEST_CASE("has_entitlement_for_sku starts_at null or absent passes when now supplied") {
    const json with_null = interaction_with(
        json::array({{{"sku_id", "sku_1"}, {"starts_at", nullptr}}}));
    CHECK(has_entitlement_for_sku(with_null, "sku_1", "2026-07-02T00:00:00.000000+00:00"));
    const json absent = interaction_with(json::array({{{"sku_id", "sku_1"}}}));
    CHECK(has_entitlement_for_sku(absent, "sku_1", "2026-07-02T00:00:00.000000+00:00"));
}

TEST_CASE("has_entitlement_for_sku starts_at check skipped when now empty") {
    const json interaction = interaction_with(
        json::array({{{"sku_id", "sku_1"}, {"starts_at", "2099-01-01T00:00:00.000000+00:00"}}}));
    CHECK(has_entitlement_for_sku(interaction, "sku_1"));
}

TEST_CASE("has_entitlement_for_sku expiry skipped when now empty") {
    const json interaction = interaction_with(
        json::array({{{"sku_id", "sku_1"}, {"ends_at", "2020-01-01T00:00:00.000000+00:00"}}}));
    // Past ends_at, but empty now_iso8601 (the default) skips the expiry check.
    CHECK(has_entitlement_for_sku(interaction, "sku_1"));
    CHECK(has_entitlement_for_sku(interaction, "sku_1", ""));
}

TEST_CASE("has_entitlement_for_sku finds a match among multiple entitlements") {
    const json interaction = interaction_with(json::array({
        {{"sku_id", "sku_a"}},
        {{"sku_id", "sku_1"}, {"deleted", true}},
        {{"sku_id", "sku_1"}},
    }));
    CHECK(has_entitlement_for_sku(interaction, "sku_1"));
    CHECK(has_entitlement_for_sku(interaction, "sku_a"));
    CHECK_FALSE(has_entitlement_for_sku(interaction, "sku_missing"));
}

TEST_CASE("premium_button has sku_id and style 6, no label or custom_id") {
    const json button = premium_button("sku_42");
    CHECK(button["type"] == 2);
    CHECK(button["style"] == 6);
    CHECK(button["sku_id"] == "sku_42");
    CHECK_FALSE(button.contains("label"));
    CHECK_FALSE(button.contains("custom_id"));
    CHECK_FALSE(button.contains("url"));
}

namespace {

// Reads response.hpp using a path derived from this test file's own location, so
// it works regardless of the absolute checkout/build prefix.
std::string read_response_header() {
    const std::string self = __FILE__;
    const std::string self_suffix = "tests/unit/cpp/test_premium.cpp";
    const std::string header_suffix = "src/include/discord_interactions/response.hpp";
    const auto pos = self.rfind(self_suffix);
    REQUIRE(pos != std::string::npos);
    const std::string header_path = self.substr(0, pos) + header_suffix;

    std::ifstream in{header_path};
    REQUIRE(in.is_open());
    std::stringstream buffer{};
    buffer << in.rdbuf();
    return buffer.str();
}

}  // namespace

TEST_CASE("response_premium_required constant is 10 and marked deprecated") {
    const std::string header = read_response_header();
    // The constant is declared with value 10.
    CHECK(header.find("inline constexpr int response_premium_required = 10;") !=
          std::string::npos);
    // ... and the line is marked DEPRECATED with a pointer to premium_button.
    const auto const_pos = header.find("response_premium_required");
    REQUIRE(const_pos != std::string::npos);
    const auto line_end = header.find('\n', const_pos);
    const std::string line = header.substr(const_pos, line_end - const_pos);
    CHECK(line.find("DEPRECATED") != std::string::npos);
    CHECK(line.find("premium_button") != std::string::npos);
}
