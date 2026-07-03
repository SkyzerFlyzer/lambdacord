#pragma once

// Paginator: a full-parity navigation row matching mature Discord frameworks
// (discord.js / discord.py pagination), rendered within this framework's
// stateless model. `paginator_row` emits ONE action row of FIVE secondary
// buttons — first / prev / counter / next / last — with correct disabled-state
// clamping at the ends and a single disabled 1-based counter in the middle.
//
// State channel: navigation targets ride in each button's custom_id via the
// T1.7 codec, `encode_custom_id(prefix, {"<target-page>"})`. The prefix routes
// the component (see routing.hpp / the message-component handler); the single
// arg is the destination page. Because the wire format is unchanged from the
// legacy 2-button row, `parse_page_after_prefix` accepts both old ("p:3") and
// new ids for free.
//
// The counter button is decorative: it is ALWAYS disabled and carries a
// non-numeric sentinel arg ("noop"), so Discord never delivers an interaction
// for it. A component handler receiving the paginator prefix must therefore
// ignore non-numeric args — `parse_page_after_prefix` deliberately keeps its
// throw-on-nonnumeric contract (it does NOT special-case "noop"), throwing
// ModuleError(validation); the handler filters, the parser stays strict.
//
// Header-only, standard library plus nlohmann/json and the framework's
// custom_id.hpp / components.hpp only (AD-2/AD-7). Consumed identically by unit
// tests and Lambdas.

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>

#include "discord_interactions/components.hpp"
#include "discord_interactions/custom_id.hpp"
#include "discord_interactions/errors.hpp"

namespace discord_interactions {

using json = nlohmann::json;

// Builds the five-button paginator action row for `page` (0-based) of
// `total_pages`. Buttons, in order:
//   0. first    ⏮  -> page 0,            disabled at the first page
//   1. prev     ◀  -> page-1 (clamped),  disabled at the first page
//   2. counter      "<page+1> / <total>", ALWAYS disabled (sentinel "noop" id)
//   3. next     ▶  -> page+1 (clamped),  disabled at the last page
//   4. last     ⏭  -> total_pages-1,     disabled at the last page
// Every button is disabled when total_pages <= 1. total_pages == 0 is treated
// as a single empty page: all disabled, counter reads "0 / 0".
inline json paginator_row(const std::string& prefix, size_t page, size_t total_pages) {
    const bool single_page = total_pages <= 1;
    const size_t last_page = total_pages == 0 ? 0 : total_pages - 1;
    const size_t prev_page = page == 0 ? 0 : page - 1;
    const size_t next_page = page >= last_page ? last_page : page + 1;

    const bool at_start = single_page || page == 0;
    const bool at_end = single_page || page >= last_page;

    const std::string counter_label =
        total_pages == 0 ? std::string{"0 / 0"}
                         : std::to_string(page + 1) + " / " + std::to_string(total_pages);

    const json components = json::array({
        button(ButtonStyle::secondary, encode_custom_id(prefix, {"0"}),
               "\xE2\x8F\xAE", at_start),  // U+23EE first
        button(ButtonStyle::secondary, encode_custom_id(prefix, {std::to_string(prev_page)}),
               "\xE2\x97\x80", at_start),  // U+25C0 prev
        button(ButtonStyle::secondary, encode_custom_id(prefix, {"noop"}),
               counter_label, true),  // counter, always disabled
        button(ButtonStyle::secondary, encode_custom_id(prefix, {std::to_string(next_page)}),
               "\xE2\x96\xB6", at_end),  // U+25B6 next
        button(ButtonStyle::secondary, encode_custom_id(prefix, {std::to_string(last_page)}),
               "\xE2\x8F\xAD", at_end),  // U+23ED last
    });

    return json::array({action_row(components)});
}

// Parses the destination page out of a paginator button's custom_id. Strict by
// design: throws ModuleError(code="paginator_unexpected_custom_id", validation)
// on a wrong prefix or a missing arg, and
// ModuleError(code="paginator_invalid_page", validation) unless the arg is a
// non-empty, full-string run of ASCII digits that fits in size_t — "3abc",
// "-1", "+1", "" and the counter's "noop" sentinel are all rejected. Callers
// must route only numeric-arg components.
inline size_t parse_page_after_prefix(const std::string& custom_id,
                                      const std::string& expected_prefix) {
    const CustomId parsed = parse_custom_id(custom_id);
    if (parsed.prefix != expected_prefix || parsed.args.empty()) {
        throw MODULE_ERROR("paginator_unexpected_custom_id", ErrorCategory::validation,
                           "unexpected paginator custom_id prefix or missing page arg");
    }
    const std::string& arg = parsed.args.front();
    if (arg.empty() || arg.find_first_not_of("0123456789") != std::string::npos) {
        throw MODULE_ERROR("paginator_invalid_page", ErrorCategory::validation,
                           "paginator page arg must be digits only");
    }
    size_t page = 0;
    for (const char ch : arg) {
        const size_t digit = static_cast<size_t>(ch - '0');
        if (page > (static_cast<size_t>(-1) - digit) / 10) {
            throw MODULE_ERROR("paginator_invalid_page", ErrorCategory::validation,
                               "paginator page arg overflows size_t");
        }
        page = page * 10 + digit;
    }
    return page;
}

}  // namespace discord_interactions
