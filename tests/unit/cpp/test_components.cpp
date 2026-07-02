#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "discord_interactions/components.hpp"
#include "discord_interactions/errors.hpp"
// NOTE: response.hpp is deliberately NOT included here. It pulls in
// aws-lambda-runtime and curl headers that are intentionally absent from the
// unit-test include path. The link_button_row backwards-compat guarantee is
// verified two ways below: (1) the components.hpp building blocks compose to the
// exact legacy JSON, and (2) response.hpp's source is grep-asserted to delegate
// to components.hpp (the same strategy test_premium.cpp uses for a header that
// cannot be included under the unit-test include path).

#include <fstream>
#include <sstream>
#include <string>

using nlohmann::json;
using discord_interactions::ButtonStyle;
using discord_interactions::button;
using discord_interactions::string_select;
using discord_interactions::select_option;
using discord_interactions::user_select;
using discord_interactions::role_select;
using discord_interactions::channel_select;
using discord_interactions::action_row;
using discord_interactions::rows;
using discord_interactions::ModuleError;
using discord_interactions::ErrorCategory;

TEST_CASE("button non-link emits custom_id with type 2 and style code") {
    const json b = button(ButtonStyle::primary, "click_me", "Click");
    CHECK(b["type"] == 2);
    CHECK(b["style"] == 1);
    CHECK(b["label"] == "Click");
    CHECK(b["custom_id"] == "click_me");
    CHECK_FALSE(b.contains("url"));
    CHECK_FALSE(b.contains("disabled"));
    CHECK_FALSE(b.contains("emoji"));
}

TEST_CASE("button secondary/success/danger style codes") {
    CHECK(button(ButtonStyle::secondary, "a", "A")["style"] == 2);
    CHECK(button(ButtonStyle::success, "b", "B")["style"] == 3);
    CHECK(button(ButtonStyle::danger, "c", "C")["style"] == 4);
}

TEST_CASE("button link emits url and no custom_id") {
    const json b = button(ButtonStyle::link, "https://example.com", "Open");
    CHECK(b["type"] == 2);
    CHECK(b["style"] == 5);
    CHECK(b["label"] == "Open");
    CHECK(b["url"] == "https://example.com");
    CHECK_FALSE(b.contains("custom_id"));
}

TEST_CASE("button disabled and emoji emitted only when non-default") {
    const json emoji = json{{"name", "fire"}, {"id", nullptr}};
    const json b = button(ButtonStyle::danger, "boom", "Boom", true, emoji);
    CHECK(b["disabled"] == true);
    CHECK(b["emoji"] == emoji);
}

TEST_CASE("button custom_id clamped to 100 chars") {
    const std::string long_id(250, 'x');
    const json b = button(ButtonStyle::primary, long_id, "L");
    const std::string clamped = b["custom_id"].get<std::string>();
    // safe_truncate keeps result within the byte budget (limits::custom_id == 100).
    CHECK(clamped.size() <= 100);
    CHECK(clamped.size() < long_id.size());
}

TEST_CASE("string_select shape with defaults omitted") {
    const json options = json::array({select_option("One", "1"), select_option("Two", "2")});
    const json s = string_select("pick", options);
    CHECK(s["type"] == 3);
    CHECK(s["custom_id"] == "pick");
    CHECK(s["options"] == options);
    CHECK_FALSE(s.contains("placeholder"));
    CHECK_FALSE(s.contains("min_values"));
    CHECK_FALSE(s.contains("max_values"));
    CHECK_FALSE(s.contains("disabled"));
}

TEST_CASE("string_select emits non-default placeholder/min/max/disabled") {
    const json options = json::array({select_option("One", "1")});
    const json s = string_select("pick", options, "Choose", 0, 3, true);
    CHECK(s["placeholder"] == "Choose");
    CHECK(s["min_values"] == 0);
    CHECK(s["max_values"] == 3);
    CHECK(s["disabled"] == true);
}

TEST_CASE("select_option shape and default omission") {
    const json plain = select_option("Label", "val");
    CHECK(plain["label"] == "Label");
    CHECK(plain["value"] == "val");
    CHECK_FALSE(plain.contains("description"));
    CHECK_FALSE(plain.contains("default"));

    const json full = select_option("Label", "val", "desc", true);
    CHECK(full["description"] == "desc");
    CHECK(full["default"] == true);
}

TEST_CASE("user_select emits type 5") {
    const json s = user_select("owner", "Pick a user");
    CHECK(s["type"] == 5);
    CHECK(s["custom_id"] == "owner");
    CHECK(s["placeholder"] == "Pick a user");
}

TEST_CASE("user_select omits placeholder when empty") {
    const json s = user_select("owner");
    CHECK(s["type"] == 5);
    CHECK_FALSE(s.contains("placeholder"));
}

TEST_CASE("role_select emits type 6") {
    const json s = role_select("roles");
    CHECK(s["type"] == 6);
    CHECK(s["custom_id"] == "roles");
    CHECK_FALSE(s.contains("placeholder"));
}

TEST_CASE("channel_select emits type 8 with channel_types") {
    const json types = json::array({0, 2});
    const json s = channel_select("chan", "Pick a channel", types);
    CHECK(s["type"] == 8);
    CHECK(s["custom_id"] == "chan");
    CHECK(s["placeholder"] == "Pick a channel");
    CHECK(s["channel_types"] == types);
}

TEST_CASE("channel_select omits channel_types when empty") {
    const json s = channel_select("chan");
    CHECK(s["type"] == 8);
    CHECK_FALSE(s.contains("channel_types"));
    CHECK_FALSE(s.contains("placeholder"));
}

TEST_CASE("action_row wraps components with type 1") {
    const json comps = json::array({button(ButtonStyle::primary, "a", "A"),
                                    button(ButtonStyle::secondary, "b", "B")});
    const json row = action_row(comps);
    CHECK(row["type"] == 1);
    CHECK(row["components"] == comps);
}

TEST_CASE("action_row allows a single select") {
    const json comps = json::array({string_select("pick", json::array())});
    const json row = action_row(comps);
    CHECK(row["type"] == 1);
    CHECK(row["components"].size() == 1);
}

TEST_CASE("action_row throws too_many_components on six buttons") {
    json comps = json::array();
    for (int i = 0; i < 6; ++i) {
        comps.push_back(button(ButtonStyle::primary, "b" + std::to_string(i), "B"));
    }
    try {
        action_row(comps);
        FAIL("expected ModuleError");
    } catch (const ModuleError& e) {
        CHECK(e.code == "too_many_components");
        CHECK(e.category == ErrorCategory::validation);
    }
}

TEST_CASE("action_row throws too_many_components on two selects") {
    const json comps = json::array({string_select("a", json::array()),
                                    user_select("b")});
    try {
        action_row(comps);
        FAIL("expected ModuleError");
    } catch (const ModuleError& e) {
        CHECK(e.code == "too_many_components");
        CHECK(e.category == ErrorCategory::validation);
    }
}

TEST_CASE("rows returns an array of the given rows") {
    const json a = action_row(json::array({button(ButtonStyle::primary, "a", "A")}));
    const json b = action_row(json::array({button(ButtonStyle::secondary, "b", "B")}));
    const json result = rows({a, b});
    REQUIRE(result.is_array());
    REQUIRE(result.size() == 2);
    CHECK(result[0] == a);
    CHECK(result[1] == b);
}

TEST_CASE("rows throws too_many_components past the five-row cap") {
    const json r = action_row(json::array({button(ButtonStyle::primary, "a", "A")}));
    try {
        rows({r, r, r, r, r, r});  // six rows
        FAIL("expected ModuleError");
    } catch (const ModuleError& e) {
        CHECK(e.code == "too_many_components");
        CHECK(e.category == ErrorCategory::validation);
    }
    // Exactly five rows is allowed.
    CHECK(rows({r, r, r, r, r}).size() == 5);
}

TEST_CASE("link_button_row legacy shape composes from components building blocks") {
    // The byte-identical legacy shape produced by response.hpp::link_button_row.
    const json legacy = json::array({{{"type", 1},
                                      {"components",
                                       json::array({{{"type", 2},
                                                     {"style", 5},
                                                     {"label", "Docs"},
                                                     {"url", "https://docs"}}})}}});
    // The refactored implementation delegates to components.hpp exactly like this.
    const json composed = json::array(
        {action_row(json::array({button(ButtonStyle::link, "https://docs", "Docs")}))});
    CHECK(composed == legacy);
    CHECK(composed.dump() == legacy.dump());
}

namespace {

// Reads response.hpp using a path derived from this test file's own location, so
// it works regardless of the absolute checkout/build prefix.
std::string read_response_header() {
    const std::string self = __FILE__;
    const std::string self_suffix = "tests/unit/cpp/test_components.cpp";
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

TEST_CASE("response.hpp link_button_row delegates to components.hpp") {
    const std::string header = read_response_header();
    CHECK(header.find("components.hpp") != std::string::npos);
    // The refactored function body composes via button()/action_row(), not the
    // old hand-rolled literal.
    const auto fn_pos = header.find("link_button_row");
    REQUIRE(fn_pos != std::string::npos);
    const std::string body = header.substr(fn_pos);
    CHECK(body.find("button(") != std::string::npos);
    CHECK(body.find("action_row(") != std::string::npos);
}
