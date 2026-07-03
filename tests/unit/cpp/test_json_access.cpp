#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "discord_interactions/json_access.hpp"

#include <cstdint>
#include <string>

using nlohmann::json;
using discord_interactions::get_if;
using discord_interactions::get_array_if;

TEST_CASE("get_if<std::string> matches only string values") {
    const json node = json{{"name", "term"}, {"count", 3}};
    REQUIRE(get_if<std::string>(node, "name").has_value());
    CHECK(get_if<std::string>(node, "name").value() == "term");
    // Present but wrong-typed: nullopt, never a throw.
    CHECK_FALSE(get_if<std::string>(node, "count").has_value());
    // Absent key.
    CHECK_FALSE(get_if<std::string>(node, "missing").has_value());
}

TEST_CASE("get_if<int> requires an integer JSON number") {
    const json node = json{{"type", 3}, {"kind", "3"}, {"ratio", 3.5}, {"flag", true}};
    REQUIRE(get_if<int>(node, "type").has_value());
    CHECK(get_if<int>(node, "type").value() == 3);
    CHECK_FALSE(get_if<int>(node, "kind").has_value());   // string "3"
    CHECK_FALSE(get_if<int>(node, "ratio").has_value());  // float, no truncation
    CHECK_FALSE(get_if<int>(node, "flag").has_value());   // bool is not a number
    CHECK(get_if<int64_t>(node, "type").value_or(0) == 3);
}

TEST_CASE("get_if<bool> requires a boolean") {
    const json node = json{{"focused", true}, {"almost", 1}, {"text", "true"}};
    REQUIRE(get_if<bool>(node, "focused").has_value());
    CHECK(get_if<bool>(node, "focused").value() == true);
    CHECK_FALSE(get_if<bool>(node, "almost").has_value());  // integer 1
    CHECK_FALSE(get_if<bool>(node, "text").has_value());
}

TEST_CASE("get_if<double> accepts any JSON number") {
    const json node = json{{"ratio", 3.5}, {"count", 3}, {"text", "3.5"}};
    CHECK(get_if<double>(node, "ratio").value_or(0.0) == doctest::Approx(3.5));
    CHECK(get_if<double>(node, "count").value_or(0.0) == doctest::Approx(3.0));
    CHECK_FALSE(get_if<double>(node, "text").has_value());
}

TEST_CASE("get_if on a non-object node returns nullopt") {
    CHECK_FALSE(get_if<std::string>(json::array(), "name").has_value());
    CHECK_FALSE(get_if<int>(json("plain string"), "type").has_value());
    CHECK_FALSE(get_if<bool>(json(), "focused").has_value());
}

TEST_CASE("get_array_if returns a pointer only for a present array") {
    const json node = json{{"options", json::array({1, 2})}, {"name", "x"}};
    const json* options = get_array_if(node, "options");
    REQUIRE(options != nullptr);
    CHECK(options->size() == 2);
    CHECK(get_array_if(node, "name") == nullptr);     // wrong type
    CHECK(get_array_if(node, "missing") == nullptr);  // absent
    CHECK(get_array_if(json::array(), "options") == nullptr);  // non-object node
}
