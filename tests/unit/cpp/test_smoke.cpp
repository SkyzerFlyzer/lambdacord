#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "discord_interactions/interaction.hpp"

using nlohmann::json;

TEST_CASE("route_suffix_from_path converts spaces to dashes") {
    CHECK(discord_interactions::route_suffix_from_path("a b") == "a-b");
}

TEST_CASE("header_value lookup is case-insensitive") {
    const json headers = json{{"X-Foo", "bar"}};
    CHECK(discord_interactions::header_value(headers, "x-foo") == "bar");
}
