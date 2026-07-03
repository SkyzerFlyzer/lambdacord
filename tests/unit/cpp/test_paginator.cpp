#include <doctest/doctest.h>

#include <string>

#include "discord_interactions/custom_id.hpp"
#include "discord_interactions/errors.hpp"
#include "discord_interactions/paginator.hpp"

using namespace discord_interactions;

namespace {

// Helpers reading the five-button row. button() (components.hpp) omits the
// "disabled" key when the button is enabled, so read it defensively.
bool is_disabled(const json& component) {
    return component.value("disabled", false);
}

const json& button_at(const json& row, std::size_t index) {
    return row[0]["components"][index];
}

}  // namespace

TEST_CASE("paginator_row is one action row of five secondary buttons") {
    const json row = paginator_row("p", 2, 5);
    REQUIRE(row.is_array());
    REQUIRE(row.size() == 1);
    CHECK(row[0]["type"] == 1);  // action row
    const json& buttons = row[0]["components"];
    REQUIRE(buttons.size() == 5);
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK(buttons[i]["type"] == 2);   // button
        CHECK(buttons[i]["style"] == 2);  // secondary
    }
}

TEST_CASE("paginator_row buttons carry the first/prev/counter/next/last glyphs") {
    const json row = paginator_row("p", 2, 5);
    CHECK(button_at(row, 0)["label"] == "\xE2\x8F\xAE");  // U+23EE first
    CHECK(button_at(row, 1)["label"] == "\xE2\x97\x80");  // U+25C0 prev
    CHECK(button_at(row, 3)["label"] == "\xE2\x96\xB6");  // U+25B6 next
    CHECK(button_at(row, 4)["label"] == "\xE2\x8F\xAD");  // U+23ED last
}

TEST_CASE("middle page enables all nav; counter always disabled") {
    const json row = paginator_row("p", 2, 5);  // last_page == 4
    CHECK(is_disabled(button_at(row, 0)) == false);  // first
    CHECK(is_disabled(button_at(row, 1)) == false);  // prev
    CHECK(is_disabled(button_at(row, 2)) == true);   // counter (always)
    CHECK(is_disabled(button_at(row, 3)) == false);  // next
    CHECK(is_disabled(button_at(row, 4)) == false);  // last
}

TEST_CASE("first page disables first and prev, keeps next and last enabled") {
    const json row = paginator_row("p", 0, 5);
    CHECK(is_disabled(button_at(row, 0)) == true);   // first
    CHECK(is_disabled(button_at(row, 1)) == true);   // prev
    CHECK(is_disabled(button_at(row, 3)) == false);  // next
    CHECK(is_disabled(button_at(row, 4)) == false);  // last
}

TEST_CASE("last page disables next and last, keeps first and prev enabled") {
    const json row = paginator_row("p", 4, 5);  // last_page == 4
    CHECK(is_disabled(button_at(row, 0)) == false);  // first
    CHECK(is_disabled(button_at(row, 1)) == false);  // prev
    CHECK(is_disabled(button_at(row, 3)) == true);   // next
    CHECK(is_disabled(button_at(row, 4)) == true);   // last
}

TEST_CASE("single page disables every button") {
    const json row = paginator_row("p", 0, 1);
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK(is_disabled(button_at(row, i)) == true);
    }
}

TEST_CASE("counter label is 1-based '<page+1> / <total_pages>'") {
    CHECK(button_at(paginator_row("p", 0, 5), 2)["label"] == "1 / 5");
    CHECK(button_at(paginator_row("p", 2, 5), 2)["label"] == "3 / 5");
    CHECK(button_at(paginator_row("p", 4, 5), 2)["label"] == "5 / 5");
    CHECK(button_at(paginator_row("p", 0, 1), 2)["label"] == "1 / 1");
}

TEST_CASE("custom_id targets: first->0, last->N-1, prev/next arithmetic") {
    const json row = paginator_row("p", 2, 5);
    CHECK(button_at(row, 0)["custom_id"] == "p:0");     // first
    CHECK(button_at(row, 1)["custom_id"] == "p:1");     // prev (page-1)
    CHECK(button_at(row, 2)["custom_id"] == "p:noop");  // counter sentinel
    CHECK(button_at(row, 3)["custom_id"] == "p:3");     // next (page+1)
    CHECK(button_at(row, 4)["custom_id"] == "p:4");     // last (N-1)
}

TEST_CASE("custom_id targets clamp at the first page") {
    const json row = paginator_row("p", 0, 5);
    CHECK(button_at(row, 0)["custom_id"] == "p:0");  // first
    CHECK(button_at(row, 1)["custom_id"] == "p:0");  // prev clamps to 0
    CHECK(button_at(row, 3)["custom_id"] == "p:1");  // next
    CHECK(button_at(row, 4)["custom_id"] == "p:4");  // last (N-1)
}

TEST_CASE("custom_id targets clamp at the last page") {
    const json row = paginator_row("p", 4, 5);
    CHECK(button_at(row, 1)["custom_id"] == "p:3");  // prev
    CHECK(button_at(row, 3)["custom_id"] == "p:4");  // next clamps to last
    CHECK(button_at(row, 4)["custom_id"] == "p:4");  // last (N-1)
}

TEST_CASE("zero total_pages renders a single disabled page with '0 / 0' counter") {
    const json row = paginator_row("p", 0, 0);
    REQUIRE(row[0]["components"].size() == 5);
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK(is_disabled(button_at(row, i)) == true);
    }
    CHECK(button_at(row, 2)["label"] == "0 / 0");
    // last_page clamps to 0 when total_pages == 0.
    CHECK(button_at(row, 4)["custom_id"] == "p:0");
}

TEST_CASE("round-trip: parse_page_after_prefix on the next button id yields page+1") {
    const json row = paginator_row("p", 2, 5);
    const std::string next_id = button_at(row, 3)["custom_id"];
    CHECK(parse_page_after_prefix(next_id, "p") == 3);

    const std::string prev_id = button_at(row, 1)["custom_id"];
    CHECK(parse_page_after_prefix(prev_id, "p") == 1);

    const std::string last_id = button_at(row, 4)["custom_id"];
    CHECK(parse_page_after_prefix(last_id, "p") == 4);

    const std::string first_id = button_at(row, 0)["custom_id"];
    CHECK(parse_page_after_prefix(first_id, "p") == 0);
}

TEST_CASE("the noop counter id is never routed: parse_page_after_prefix throws on it") {
    // The counter carries a non-numeric sentinel arg ("noop"). A component
    // handler receiving the paginator prefix must ignore non-numeric args; the
    // ModuleError(validation) here pins that parse_page_after_prefix keeps its
    // throw-on-nonnumeric contract rather than silently returning a bogus page.
    const json row = paginator_row("p", 2, 5);
    const std::string noop_id = button_at(row, 2)["custom_id"];
    try {
        parse_page_after_prefix(noop_id, "p");
        FAIL("expected ModuleError");
    } catch (const ModuleError& e) {
        CHECK(e.category == ErrorCategory::validation);
    }
}

TEST_CASE("parse_page_after_prefix throws ModuleError(validation) on a wrong prefix") {
    try {
        parse_page_after_prefix("q:3", "p");
        FAIL("expected ModuleError");
    } catch (const ModuleError& e) {
        CHECK(e.category == ErrorCategory::validation);
    }
    // A prefix with no arg at all is also a validation error.
    CHECK_THROWS_AS(parse_page_after_prefix("p", "p"), ModuleError);
}

TEST_CASE("parse_page_after_prefix is strict full-string digits-only") {
    // Trailing garbage must not parse as page 3 (std::stoul would accept it).
    CHECK_THROWS_AS(parse_page_after_prefix("p:3abc", "p"), ModuleError);
    // A sign is not a digit; "-1" must not wrap to a huge page.
    CHECK_THROWS_AS(parse_page_after_prefix("p:-1", "p"), ModuleError);
    CHECK_THROWS_AS(parse_page_after_prefix("p:+1", "p"), ModuleError);
    // An empty arg is rejected too.
    CHECK_THROWS_AS(parse_page_after_prefix("p:", "p"), ModuleError);
    // Plain digits still parse.
    CHECK(parse_page_after_prefix("p:12", "p") == 12);
}
