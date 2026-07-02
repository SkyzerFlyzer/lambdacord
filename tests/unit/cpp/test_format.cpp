#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#include "discord_interactions/errors.hpp"
#include "discord_interactions/format.hpp"

using namespace discord_interactions;

// U+200B ZERO WIDTH SPACE, UTF-8 bytes E2 80 8B. code_block() inserts one of
// these between consecutive backticks in the body so an embedded ``` fence can
// never terminate the block early (documented strategy — see format.hpp).
static const std::string ZWSP = "\xE2\x80\x8B";

TEST_CASE("is_snowflake rejects 16-digit strings") {
    CHECK(is_snowflake("1234567890123456") == false);  // 16 digits
}

TEST_CASE("is_snowflake accepts 17-digit strings") {
    CHECK(is_snowflake("12345678901234567") == true);  // 17 digits
}

TEST_CASE("is_snowflake accepts 20-digit strings") {
    CHECK(is_snowflake("12345678901234567890") == true);  // 20 digits
}

TEST_CASE("is_snowflake rejects 21-digit strings") {
    CHECK(is_snowflake("123456789012345678901") == false);  // 21 digits
}

TEST_CASE("is_snowflake rejects non-numeric input") {
    CHECK(is_snowflake("12345678901234567x") == false);
    CHECK(is_snowflake("abcdefghijklmnopq") == false);
    CHECK(is_snowflake(" 1234567890123456") == false);  // leading space
    CHECK(is_snowflake("") == false);                    // empty
}

TEST_CASE("snowflake_timestamp_ms matches Discord's documented example") {
    // Discord docs: snowflake 175928847299117063 unpacks to 1462015105796 ms.
    //   175928847299117063 >> 22 = 41944705796
    //   41944705796 + 1420070400000 (Discord epoch) = 1462015105796
    CHECK(snowflake_timestamp_ms("175928847299117063") == INT64_C(1462015105796));
}

TEST_CASE("snowflake_timestamp_ms of the Discord epoch base snowflake is the epoch") {
    // A snowflake whose upper bits are all zero (id 0) -> pure Discord epoch.
    CHECK(snowflake_timestamp_ms("00000000000000000") == INT64_C(1420070400000));
}

TEST_CASE("snowflake_timestamp_ms throws ModuleError(validation) on bad input") {
    bool threw = false;
    try {
        snowflake_timestamp_ms("not-a-snowflake");
    } catch (const ModuleError& e) {
        threw = true;
        CHECK(e.category == ErrorCategory::validation);
    }
    CHECK(threw == true);
}

TEST_CASE("user_mention wraps id in <@...>") {
    CHECK(user_mention("123456789012345678") == "<@123456789012345678>");
}

TEST_CASE("channel_mention wraps id in <#...>") {
    CHECK(channel_mention("123456789012345678") == "<#123456789012345678>");
}

TEST_CASE("role_mention wraps id in <@&...>") {
    CHECK(role_mention("123456789012345678") == "<@&123456789012345678>");
}

TEST_CASE("discord_timestamp emits the correct style letter for each style") {
    const int64_t s = 1462015105;
    CHECK(discord_timestamp(s, TimestampStyle::short_time) == "<t:1462015105:t>");
    CHECK(discord_timestamp(s, TimestampStyle::long_time) == "<t:1462015105:T>");
    CHECK(discord_timestamp(s, TimestampStyle::short_date) == "<t:1462015105:d>");
    CHECK(discord_timestamp(s, TimestampStyle::long_date) == "<t:1462015105:D>");
    CHECK(discord_timestamp(s, TimestampStyle::short_datetime) == "<t:1462015105:f>");
    CHECK(discord_timestamp(s, TimestampStyle::long_datetime) == "<t:1462015105:F>");
    CHECK(discord_timestamp(s, TimestampStyle::relative) == "<t:1462015105:R>");
}

TEST_CASE("escape_markdown backslashes every special character") {
    CHECK(escape_markdown("*") == "\\*");
    CHECK(escape_markdown("_") == "\\_");
    CHECK(escape_markdown("~") == "\\~");
    CHECK(escape_markdown("`") == "\\`");
    CHECK(escape_markdown("|") == "\\|");
    CHECK(escape_markdown(">") == "\\>");
    CHECK(escape_markdown("#") == "\\#");
    CHECK(escape_markdown("-") == "\\-");
    CHECK(escape_markdown("plain text") == "plain text");
    CHECK(escape_markdown("**bold**") == "\\*\\*bold\\*\\*");
}

TEST_CASE("escape_markdown re-escapes already-escaped input (not idempotent)") {
    // The backslash itself is not a tracked special char, so a pre-existing
    // backslash is left untouched and the following '*' is escaped again.
    // Input  : \*  (backslash, asterisk)
    // Output : \\* (backslash, backslash, asterisk)
    CHECK(escape_markdown("\\*") == "\\\\*");
}

TEST_CASE("code_block without a language fences the body plainly") {
    CHECK(code_block("hello") == "```\nhello\n```");
}

TEST_CASE("code_block with a language sets the fence info string") {
    CHECK(code_block("print(1)", "python") == "```python\nprint(1)\n```");
}

TEST_CASE("code_block breaks embedded backtick runs with a zero-width space") {
    // Body "a```b": the run of three backticks gets a ZWSP inserted between each
    // adjacent pair so it cannot close the fence.
    const std::string expected = "```\na`" + ZWSP + "`" + ZWSP + "`b\n```";
    CHECK(code_block("a```b") == expected);
}

TEST_CASE("code_block leaves a single backtick untouched") {
    CHECK(code_block("a`b") == "```\na`b\n```");
}
