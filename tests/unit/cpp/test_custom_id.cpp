#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "discord_interactions/custom_id.hpp"
#include "discord_interactions/errors.hpp"
#include "discord_interactions/paginator.hpp"

using namespace discord_interactions;

TEST_CASE("encode_custom_id joins prefix and args with ':'") {
    CHECK(encode_custom_id("page", {"3", "user42"}) == "page:3:user42");
}

TEST_CASE("encode/parse round-trips a multi-arg custom_id") {
    const std::string raw = encode_custom_id("page", {"3", "user42"});
    const CustomId parsed = parse_custom_id(raw);
    CHECK(parsed.prefix == "page");
    REQUIRE(parsed.args.size() == 2);
    CHECK(parsed.args[0] == "3");
    CHECK(parsed.args[1] == "user42");
}

TEST_CASE("prefix-only encodes to the bare prefix and parses to empty args") {
    CHECK(encode_custom_id("refresh", {}) == "refresh");
    const CustomId parsed = parse_custom_id("refresh");
    CHECK(parsed.prefix == "refresh");
    CHECK(parsed.args.empty());
}

TEST_CASE("parse preserves empty arg elements: \"p::x\" -> args {\"\", \"x\"}") {
    const CustomId parsed = parse_custom_id("p::x");
    CHECK(parsed.prefix == "p");
    REQUIRE(parsed.args.size() == 2);
    CHECK(parsed.args[0] == "");
    CHECK(parsed.args[1] == "x");
}

TEST_CASE("encode with an empty-string arg round-trips through parse") {
    const std::string raw = encode_custom_id("p", {"", "x"});
    CHECK(raw == "p::x");
    const CustomId parsed = parse_custom_id(raw);
    CHECK(parsed.prefix == "p");
    REQUIRE(parsed.args.size() == 2);
    CHECK(parsed.args[0] == "");
    CHECK(parsed.args[1] == "x");
}

TEST_CASE("encode throws ModuleError(validation) when an arg contains ':'") {
    bool threw = false;
    try {
        encode_custom_id("page", {"a:b"});
    } catch (const ModuleError& e) {
        threw = true;
        CHECK(e.category == ErrorCategory::validation);
    }
    CHECK(threw == true);
}

TEST_CASE("encode throws ModuleError(validation) when the prefix contains ':'") {
    bool threw = false;
    try {
        encode_custom_id("pa:ge", {"1"});
    } catch (const ModuleError& e) {
        threw = true;
        CHECK(e.category == ErrorCategory::validation);
    }
    CHECK(threw == true);
}

TEST_CASE("encode throws ModuleError(validation) when total length exceeds 100") {
    const std::string big(200, 'x');
    bool threw = false;
    try {
        encode_custom_id("p", {big});
    } catch (const ModuleError& e) {
        threw = true;
        CHECK(e.category == ErrorCategory::validation);
    }
    CHECK(threw == true);
}

TEST_CASE("encode allows a custom_id of exactly 100 characters") {
    // prefix "p" + ":" + 98 chars = 100 total.
    const std::string arg(98, 'a');
    const std::string raw = encode_custom_id("p", {arg});
    CHECK(raw.size() == 100);
}

TEST_CASE("encode throws ModuleError(validation) on an empty prefix") {
    bool threw = false;
    try {
        encode_custom_id("", {"1"});
    } catch (const ModuleError& e) {
        threw = true;
        CHECK(e.category == ErrorCategory::validation);
    }
    CHECK(threw == true);
}

TEST_CASE("parse throws ModuleError(validation) on an empty prefix") {
    bool threw_empty = false;
    try {
        parse_custom_id("");
    } catch (const ModuleError& e) {
        threw_empty = true;
        CHECK(e.category == ErrorCategory::validation);
    }
    CHECK(threw_empty == true);

    bool threw_leading = false;
    try {
        parse_custom_id(":3");  // empty prefix before the first ':'
    } catch (const ModuleError& e) {
        threw_leading = true;
        CHECK(e.category == ErrorCategory::validation);
    }
    CHECK(threw_leading == true);
}

TEST_CASE("parse of a legacy paginator id \"p:3\" yields prefix p and arg 3") {
    const CustomId parsed = parse_custom_id("p:3");
    CHECK(parsed.prefix == "p");
    REQUIRE(parsed.args.size() == 1);
    CHECK(parsed.args[0] == "3");
}

// ---- Paginator regression: output must be byte-identical after the refactor ----

TEST_CASE("paginator_row output is byte-identical for a mid-page case") {
    // page 2 of 5 (last_page = 4): Previous -> p:1 enabled, Next -> p:3 enabled.
    const json expected = json::parse(R"([
        {"type":1,"components":[
            {"type":2,"style":2,"label":"Previous","custom_id":"p:1","disabled":false},
            {"type":2,"style":2,"label":"Next","custom_id":"p:3","disabled":false}
        ]}
    ])");
    const json actual = paginator_row("p", 2, 5);
    CHECK(actual == expected);
    // Byte-identical serialization (nlohmann sorts object keys deterministically).
    CHECK(actual.dump() == expected.dump());
}

TEST_CASE("paginator_row clamps at the first and last page") {
    const json first = paginator_row("p", 0, 5);
    CHECK(first[0]["components"][0]["disabled"] == true);   // Previous disabled
    CHECK(first[0]["components"][0]["custom_id"] == "p:0");
    CHECK(first[0]["components"][1]["disabled"] == false);  // Next enabled
    CHECK(first[0]["components"][1]["custom_id"] == "p:1");

    const json last = paginator_row("p", 4, 5);
    CHECK(last[0]["components"][0]["disabled"] == false);   // Previous enabled
    CHECK(last[0]["components"][0]["custom_id"] == "p:3");
    CHECK(last[0]["components"][1]["disabled"] == true);    // Next disabled
    CHECK(last[0]["components"][1]["custom_id"] == "p:4");
}

TEST_CASE("parse_page_after_prefix returns the page number unchanged") {
    CHECK(parse_page_after_prefix("p:3", "p") == 3);
    CHECK(parse_page_after_prefix("page:0", "page") == 0);
}

TEST_CASE("parse_page_after_prefix still throws on a wrong prefix") {
    CHECK_THROWS(parse_page_after_prefix("q:3", "p"));
    CHECK_THROWS(parse_page_after_prefix("p", "p"));  // no page component
}
