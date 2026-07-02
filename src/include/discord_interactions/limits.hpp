#pragma once

// Authoritative Discord interaction/message limits plus UTF-8-safe truncation.
//
// These constants mirror the documented maximums from Discord's API reference
// (messages, embeds, components, modals, autocomplete). They are the single
// source of truth for every builder header in this framework so limit values
// are never duplicated ad hoc across Lambdas.
//
// Header-only, no third-party runtime dependencies (AD-2/AD-7): only the C++
// standard library. Consumed identically by unit tests and Lambdas.

#include <cstddef>
#include <string>

namespace discord_interactions {

namespace limits {

// Message content.
inline constexpr std::size_t content = 2000;

// Embeds (per-embed field limits and the combined 6000-character cap).
inline constexpr std::size_t embed_title = 256;
inline constexpr std::size_t embed_description = 4096;
inline constexpr std::size_t embed_fields = 25;
inline constexpr std::size_t embed_field_name = 256;
inline constexpr std::size_t embed_field_value = 1024;
inline constexpr std::size_t embed_footer = 2048;
inline constexpr std::size_t embed_author = 256;
inline constexpr std::size_t embed_total = 6000;
inline constexpr std::size_t embeds_per_message = 10;

// Message components v1 (action rows, buttons, selects).
inline constexpr std::size_t action_rows = 5;
inline constexpr std::size_t buttons_per_row = 5;
inline constexpr std::size_t select_options = 25;

// Autocomplete choices.
inline constexpr std::size_t autocomplete_choices = 25;
inline constexpr std::size_t choice_name = 100;

// Custom ids (the framework's only free state channel).
inline constexpr std::size_t custom_id = 100;

// Modals and text inputs.
inline constexpr std::size_t modal_title = 45;
inline constexpr std::size_t text_input_label = 45;
inline constexpr std::size_t text_input_value = 4000;

// Components v2 message layout.
inline constexpr std::size_t components_per_message = 40;
inline constexpr std::size_t text_display_content = 4000;

}  // namespace limits

// UTF-8-safe truncation.
//
// If `text` already fits within `max_bytes`, it is returned unchanged. When a
// cut is required, the result is trimmed so it never splits a multibyte UTF-8
// sequence and the ellipsis character "…" (U+2026, 3 bytes) is appended,
// counted within `max_bytes`. If `max_bytes` is smaller than the 3-byte
// ellipsis, no safe truncated form exists and an empty string is returned.
inline std::string safe_truncate(const std::string& text, std::size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return text;
    }

    // "…" U+2026 in UTF-8. A truncated result must reserve room for it.
    static const std::string ellipsis = "\xE2\x80\xA6";
    if (max_bytes < ellipsis.size()) {
        return std::string{};
    }

    // Bytes of original content we may keep before appending the ellipsis.
    std::size_t keep = max_bytes - ellipsis.size();

    // Retreat `keep` to a UTF-8 codepoint boundary: continuation bytes match
    // 10xxxxxx (0x80..0xBF). Never split a multibyte sequence.
    while (keep > 0 &&
           (static_cast<unsigned char>(text[keep]) & 0xC0) == 0x80) {
        --keep;
    }

    std::string result{};
    result.reserve(keep + ellipsis.size());
    result.append(text, 0, keep);
    result.append(ellipsis);
    return result;
}

}  // namespace discord_interactions
