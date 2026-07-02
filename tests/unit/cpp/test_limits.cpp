#include <doctest/doctest.h>

#include <cstddef>
#include <string>
#include <type_traits>

#include "discord_interactions/limits.hpp"

using discord_interactions::safe_truncate;
namespace limits = discord_interactions::limits;

namespace {

// The ellipsis character U+2026 encodes to these three bytes in UTF-8.
const std::string kEllipsis = "\xE2\x80\xA6";

// Verify every byte of a UTF-8 string forms complete, well-formed sequences.
// Returns false if a lead byte announces a multibyte sequence that is cut off
// or if a continuation byte appears without a lead byte.
bool is_valid_utf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 0;
        if (c < 0x80) {
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
        } else {
            return false;  // invalid lead byte / stray continuation
        }
        if (i + len > s.size()) {
            return false;  // truncated multibyte sequence
        }
        for (size_t k = 1; k < len; ++k) {
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
                return false;  // missing continuation byte
            }
        }
        i += len;
    }
    return true;
}

}  // namespace

TEST_CASE("safe_truncate passes through text shorter than the limit") {
    const std::string text = "hello";
    CHECK(safe_truncate(text, 100) == "hello");
}

TEST_CASE("safe_truncate passes through text at exactly the limit") {
    const std::string text = "hello";
    CHECK(safe_truncate(text, 5) == "hello");
}

TEST_CASE("safe_truncate cuts ASCII and appends the ellipsis within the budget") {
    const std::string text = "abcdefghij";  // 10 bytes
    const std::string out = safe_truncate(text, 6);
    // 6-byte budget: 3 bytes ellipsis + up to 3 bytes of content.
    CHECK(out == "abc" + kEllipsis);
    CHECK(out.size() <= 6);
    CHECK(is_valid_utf8(out));
}

TEST_CASE("safe_truncate never splits a 4-byte emoji straddling the boundary") {
    // Four grinning-face emoji, 4 bytes each = 16 bytes.
    const std::string emoji = "\xF0\x9F\x98\x80";  // U+1F600
    const std::string text = emoji + emoji + emoji + emoji;
    // Budget 10: ellipsis is 3, leaving 7 bytes for content. One emoji is 4
    // bytes; two would be 8 (> 7), so exactly one emoji must survive.
    const std::string out = safe_truncate(text, 10);
    CHECK(out == emoji + kEllipsis);
    CHECK(out.size() <= 10);
    CHECK(is_valid_utf8(out));
}

TEST_CASE("safe_truncate returns empty when a cut is needed but budget < ellipsis") {
    const std::string text = "abcdef";
    // A cut is required (text longer than budget) but 2 bytes cannot hold the
    // 3-byte ellipsis, so nothing safe can be emitted.
    const std::string out = safe_truncate(text, 2);
    CHECK(out.empty());
}

TEST_CASE("safe_truncate returns empty for a zero-byte budget") {
    CHECK(safe_truncate("anything", 0).empty());
}

TEST_CASE("safe_truncate passes through empty input regardless of budget") {
    CHECK(safe_truncate("", 0) == "");
    CHECK(safe_truncate("", 100) == "");
}

TEST_CASE("safe_truncate cuts a multi-emoji string mid-sequence and stays valid") {
    const std::string emoji = "\xF0\x9F\x98\x80";  // 4 bytes
    const std::string text = emoji + emoji + emoji;  // 12 bytes
    // Budget 8: ellipsis 3, leaving 5 for content. One emoji (4) fits, two (8)
    // do not. The cut lands mid-emoji and must retreat to the codepoint start.
    const std::string out = safe_truncate(text, 8);
    CHECK(out == emoji + kEllipsis);
    CHECK(out.size() <= 8);
    CHECK(is_valid_utf8(out));
}

TEST_CASE("safe_truncate cutting exactly at a codepoint boundary keeps whole chars") {
    // Two-byte characters (U+00E9 'é'), three of them = 6 bytes.
    const std::string e = "\xC3\xA9";
    const std::string text = e + e + e;  // 6 bytes, three chars
    // Budget 5: ellipsis 3, leaving 2 bytes = exactly one 2-byte char.
    const std::string out = safe_truncate(text, 5);
    CHECK(out == e + kEllipsis);
    CHECK(out.size() <= 5);
    CHECK(is_valid_utf8(out));
}

TEST_CASE("safe_truncate emits only the ellipsis when budget equals ellipsis size") {
    const std::string text = "abcdef";
    const std::string out = safe_truncate(text, 3);
    CHECK(out == kEllipsis);
    CHECK(out.size() == 3);
    CHECK(is_valid_utf8(out));
}

TEST_CASE("limits constants match Discord's documented values") {
    CHECK(limits::content == 2000);
    CHECK(limits::embed_title == 256);
    CHECK(limits::embed_description == 4096);
    CHECK(limits::embed_fields == 25);
    CHECK(limits::embed_field_name == 256);
    CHECK(limits::embed_field_value == 1024);
    CHECK(limits::embed_footer == 2048);
    CHECK(limits::embed_author == 256);
    CHECK(limits::embed_total == 6000);
    CHECK(limits::embeds_per_message == 10);
    CHECK(limits::action_rows == 5);
    CHECK(limits::buttons_per_row == 5);
    CHECK(limits::select_options == 25);
    CHECK(limits::autocomplete_choices == 25);
    CHECK(limits::choice_name == 100);
    CHECK(limits::custom_id == 100);
    CHECK(limits::modal_title == 45);
    CHECK(limits::text_input_label == 45);
    CHECK(limits::text_input_value == 4000);
    CHECK(limits::components_per_message == 40);
    CHECK(limits::text_display_content == 4000);
    // Modal top-level cap is 5 (NOT the 40-component message cap).
    CHECK(limits::modal_components == 5);
    CHECK(limits::button_label == 80);
    CHECK(limits::select_option_field == 100);
    CHECK(limits::label_description == 100);
    CHECK(limits::text_input_placeholder == 100);
    CHECK(limits::radio_options_min == 2);
    CHECK(limits::radio_options_max == 10);
    CHECK(limits::checkbox_group_max_values == 10);
}

TEST_CASE("limits constants are constexpr size_t usable at compile time") {
    static_assert(limits::embed_total == 6000, "embed_total must be 6000");
    static_assert(limits::components_per_message == 40,
                  "components_per_message must be 40");
    static_assert(std::is_same<decltype(limits::content), const std::size_t>::value,
                  "limits constants must be size_t");
    CHECK(true);
}
