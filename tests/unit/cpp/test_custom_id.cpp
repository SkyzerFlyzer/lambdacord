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

// ---- Paginator custom_id wire-format regression (five-button row, T4.4) ----
// The paginator shape changed from 2 buttons (Previous/Next) to 5
// (first/prev/counter/next/last) in T4.4; full behavioral coverage lives in
// test_paginator.cpp. These cases pin only that the wire format the custom_id
// codec round-trips is unchanged: prev/next arithmetic still encode via
// encode_custom_id(prefix, {"<page>"}).

TEST_CASE("paginator_row prev/next custom_ids use the encode_custom_id wire format") {
    // page 2 of 5 (last_page = 4): prev -> p:1, next -> p:3.
    const json row = paginator_row("p", 2, 5);
    REQUIRE(row[0]["components"].size() == 5);
    CHECK(row[0]["components"][1]["custom_id"] == "p:1");  // prev
    CHECK(row[0]["components"][3]["custom_id"] == "p:3");  // next
}

TEST_CASE("paginator_row clamps prev/next custom_ids at the first and last page") {
    const json first = paginator_row("p", 0, 5);
    CHECK(first[0]["components"][1]["custom_id"] == "p:0");  // prev clamps to 0
    CHECK(first[0]["components"][3]["custom_id"] == "p:1");  // next

    const json last = paginator_row("p", 4, 5);
    CHECK(last[0]["components"][1]["custom_id"] == "p:3");   // prev
    CHECK(last[0]["components"][3]["custom_id"] == "p:4");   // next clamps to last
}

TEST_CASE("parse_page_after_prefix returns the page number unchanged") {
    CHECK(parse_page_after_prefix("p:3", "p") == 3);
    CHECK(parse_page_after_prefix("page:0", "page") == 0);
}

TEST_CASE("parse_page_after_prefix still throws on a wrong prefix") {
    CHECK_THROWS(parse_page_after_prefix("q:3", "p"));
    CHECK_THROWS(parse_page_after_prefix("p", "p"));  // no page component
}
