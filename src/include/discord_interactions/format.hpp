#pragma once

// Snowflake, mention, and Discord message-formatting utilities — the utility
// belt every Discord framework ships (discord.js SnowflakeUtil / time() /
// userMention(), discord.py utils). Header-only, namespace discord_interactions
// (AD-2), no third-party runtime dependency beyond the standard library.
//
// Discord snowflakes pack a millisecond timestamp in their high bits:
//   timestamp_ms = (snowflake >> 22) + DISCORD_EPOCH_MS
// where DISCORD_EPOCH_MS = 1420070400000 (2015-01-01T00:00:00Z).
//
// code_block() embedded-backtick strategy (documented, tested in
// test_format.cpp): consecutive backticks in the body are separated by a
// U+200B ZERO WIDTH SPACE so that no run of backticks can prematurely close the
// ``` fence. The zero-width space is invisible in the rendered Discord message
// but structurally prevents fence escape. Single, non-adjacent backticks are
// left untouched.

#include <cstdint>
#include <string>

#include "discord_interactions/errors.hpp"

namespace discord_interactions {

// Discord epoch: 2015-01-01T00:00:00 UTC, in milliseconds.
inline constexpr int64_t discord_epoch_ms = INT64_C(1420070400000);

// A snowflake is a 17-to-20 digit decimal string (Discord ids currently fall in
// this range). Returns false for anything shorter, longer, empty, or containing
// a non-digit (including surrounding whitespace or a sign).
inline bool is_snowflake(const std::string& value) {
    const std::size_t length = value.size();
    if (length < 17 || length > 20) {
        return false;
    }
    for (const char c : value) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

// Extracts the creation timestamp (ms since Unix epoch) encoded in a snowflake.
// Throws ModuleError(category=validation) when the input is not a valid
// snowflake. The value is parsed as an unsigned 64-bit integer before shifting.
inline int64_t snowflake_timestamp_ms(const std::string& snowflake) {
    if (!is_snowflake(snowflake)) {
        throw MODULE_ERROR("invalid_snowflake", ErrorCategory::validation,
                           "value is not a valid Discord snowflake");
    }
    std::uint64_t parsed{};
    try {
        parsed = std::stoull(snowflake);
    } catch (const std::exception&) {
        throw MODULE_ERROR("invalid_snowflake", ErrorCategory::validation,
                           "snowflake could not be parsed as an unsigned integer");
    }
    return static_cast<int64_t>(parsed >> 22) + discord_epoch_ms;
}

// Mention helpers. Discord accepts these literal forms in message content.
inline std::string user_mention(const std::string& id) {
    return "<@" + id + ">";
}

inline std::string channel_mention(const std::string& id) {
    return "<#" + id + ">";
}

inline std::string role_mention(const std::string& id) {
    return "<@&" + id + ">";
}

// Discord timestamp display styles. Each maps to a single format letter in the
// <t:SECONDS:X> markup.
enum class TimestampStyle {
    short_time,      // t  16:20
    long_time,       // T  16:20:30
    short_date,      // d  20/04/2021
    long_date,       // D  20 April 2021
    short_datetime,  // f  20 April 2021 16:20
    long_datetime,   // F  Tuesday, 20 April 2021 16:20
    relative,        // R  2 months ago
};

inline char timestamp_style_letter(TimestampStyle style) {
    switch (style) {
    case TimestampStyle::short_time:
        return 't';
    case TimestampStyle::long_time:
        return 'T';
    case TimestampStyle::short_date:
        return 'd';
    case TimestampStyle::long_date:
        return 'D';
    case TimestampStyle::short_datetime:
        return 'f';
    case TimestampStyle::long_datetime:
        return 'F';
    case TimestampStyle::relative:
        return 'R';
    }
    return 'f';
}

// Renders Discord's dynamic timestamp markup: <t:SECONDS:X>. `unix_seconds` is
// seconds since the Unix epoch (not the Discord epoch).
inline std::string discord_timestamp(int64_t unix_seconds, TimestampStyle style) {
    std::string result{};
    result += "<t:";
    result += std::to_string(unix_seconds);
    result += ':';
    result += timestamp_style_letter(style);
    result += '>';
    return result;
}

// Backslash-escapes the Discord markdown control characters: * _ ~ ` | > # -.
// Note: the backslash itself is not tracked, so escaping is intentionally not
// idempotent — a pre-existing backslash is left as-is and the following control
// character is escaped again.
inline std::string escape_markdown(const std::string& text) {
    std::string result{};
    result.reserve(text.size());
    for (const char c : text) {
        switch (c) {
        case '*':
        case '_':
        case '~':
        case '`':
        case '|':
        case '>':
        case '#':
        case '-':
            result += '\\';
            break;
        default:
            break;
        }
        result += c;
    }
    return result;
}

// Wraps `body` in a triple-backtick code fence with an optional language info
// string. Consecutive backticks inside `body` are separated by a U+200B ZERO
// WIDTH SPACE so an embedded ``` cannot close the fence early (see file header).
inline std::string code_block(const std::string& body, const std::string& lang = "") {
    // U+200B ZERO WIDTH SPACE (UTF-8 E2 80 8B).
    static const char zero_width_space[] = "\xE2\x80\x8B";

    std::string sanitized{};
    sanitized.reserve(body.size());
    bool previous_was_backtick{};
    for (const char c : body) {
        if (c == '`' && previous_was_backtick) {
            sanitized += zero_width_space;
        }
        sanitized += c;
        previous_was_backtick = (c == '`');
    }

    std::string result{};
    result += "```";
    result += lang;
    result += '\n';
    result += sanitized;
    result += "\n```";
    return result;
}

}  // namespace discord_interactions
