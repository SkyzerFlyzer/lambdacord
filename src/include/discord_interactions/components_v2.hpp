#pragma once

// Components V2 message layout builders (Discord message flag 1 << 15):
// container / section / text-display and the surrounding layout primitives —
// parity with discord.js's ContainerBuilder / TextDisplayBuilder /
// SectionBuilder. Per Discord's component reference this surface is stable and
// documented (component types 9-14, 17); it is not experimental.
//
// IMPORTANT: A Components V2 message cannot carry `content` or `embeds` — the
// whole message body is expressed through components. `components_v2_message`
// therefore owns the entire payload shape ({flags, components}) so callers
// cannot accidentally mix `content`/`embeds` into a CV2 message. Interactive
// components from components.hpp (buttons, selects, and action rows) remain
// legal children inside containers and sections, so those factories compose
// directly here.
//
// Header-only, standard library plus nlohmann/json and the framework's
// limits.hpp / errors.hpp only (AD-2/AD-7). Consumed identically by unit tests
// and Lambdas.
//
// Component type codes emitted here follow Discord's reference:
//   section 9, text display 10, thumbnail 11, media gallery 12,
//   file 13, separator 14, container 17.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>

#include "discord_interactions/errors.hpp"
#include "discord_interactions/limits.hpp"

namespace discord_interactions {

using json = nlohmann::json;

// The message flag that marks a payload as Components V2 (IS_COMPONENTS_V2).
inline constexpr int message_flag_components_v2 = 1 << 15;

// A text display block (component type 10). Content is clamped through
// safe_truncate to limits::text_display_content (4000).
//
// NOTE: this 4000-character clamp is PER-COMPONENT and deliberately
// conservative. Discord's combined-per-message text budget across all text
// displays is not currently documented in the component reference, so it is
// deliberately NOT enforced here — very text-heavy multi-display messages may
// still be rejected by Discord's API.
inline json text_display(const std::string& content) {
    json result = json::object();
    result["type"] = 10;
    result["content"] = safe_truncate(content, limits::text_display_content);
    return result;
}

// A thumbnail accessory (component type 11). `description` and `spoiler` are
// emitted only when non-default.
inline json thumbnail_component(const std::string& media_url,
                                const std::string& description = "",
                                bool spoiler = false) {
    json result = json::object();
    result["type"] = 11;
    result["media"] = json::object();
    result["media"]["url"] = media_url;
    if (!description.empty()) {
        result["description"] = description;
    }
    if (spoiler) {
        result["spoiler"] = true;
    }
    return result;
}

// A section (component type 9): one to three text displays with an accessory
// (a thumbnail or a button from components.hpp). Throws
// ModuleError(category=validation) on zero or more than three text displays.
inline json section(const json& text_displays, const json& accessory) {
    const std::size_t count = text_displays.is_array() ? text_displays.size() : 0;
    if (count < 1 || count > 3) {
        throw MODULE_ERROR("invalid_section", ErrorCategory::validation,
                           "section requires 1 to 3 text displays, got " +
                               std::to_string(count));
    }
    json result = json::object();
    result["type"] = 9;
    result["components"] = text_displays;
    result["accessory"] = accessory;
    return result;
}

// A media gallery (component type 12) of 1 to 10 items, each an object of the
// form {media:{url}, description?, spoiler?}. Throws
// ModuleError(category=validation) outside that range.
inline json media_gallery(const json& items) {
    const std::size_t count = items.is_array() ? items.size() : 0;
    if (count < 1 || count > 10) {
        throw MODULE_ERROR("invalid_media_gallery", ErrorCategory::validation,
                           "media gallery requires 1 to 10 items, got " +
                               std::to_string(count));
    }
    json result = json::object();
    result["type"] = 12;
    result["items"] = items;
    return result;
}

// A file component (component type 13). `spoiler` is emitted only when true.
inline json file_component(const std::string& attachment_url, bool spoiler = false) {
    json result = json::object();
    result["type"] = 13;
    result["file"] = json::object();
    result["file"]["url"] = attachment_url;
    if (spoiler) {
        result["spoiler"] = true;
    }
    return result;
}

// A separator (component type 14). `spacing` must be 1 (small) or 2 (large);
// any other value throws ModuleError(category=validation).
inline json separator(bool divider = true, int spacing = 1) {
    if (spacing != 1 && spacing != 2) {
        throw MODULE_ERROR("invalid_separator", ErrorCategory::validation,
                           "separator spacing must be 1 or 2, got " +
                               std::to_string(spacing));
    }
    json result = json::object();
    result["type"] = 14;
    result["divider"] = divider;
    result["spacing"] = spacing;
    return result;
}

// A container (component type 17) wrapping a component array. `accent_color`
// is emitted only when set; `spoiler` only when true.
inline json container(const json& components,
                      std::optional<uint32_t> accent_color = std::nullopt,
                      bool spoiler = false) {
    json result = json::object();
    result["type"] = 17;
    result["components"] = components;
    if (accent_color.has_value()) {
        result["accent_color"] = accent_color.value();
    }
    if (spoiler) {
        result["spoiler"] = true;
    }
    return result;
}

namespace detail {

// Recursive component count: every node counts as one, plus every child under
// its `components` array and its `accessory` (sections). This mirrors the way
// Discord counts components toward the per-message limit — a container and its
// children, and a section's text displays and accessory, all count.
inline std::size_t count_components_v2(const json& node) {
    std::size_t total = 1;
    if (node.is_object()) {
        const auto components = node.find("components");
        if (components != node.end() && components->is_array()) {
            for (const auto& child : *components) {
                total += count_components_v2(child);
            }
        }
        const auto accessory = node.find("accessory");
        if (accessory != node.end() && accessory->is_object()) {
            total += count_components_v2(*accessory);
        }
    }
    return total;
}

}  // namespace detail

// Assembles a top-level Components V2 message: {flags, components}. A CV2
// message never carries `content` or `embeds` (see header comment). Validates
// the recursive component count (containers and their children, sections and
// their text displays/accessory all count) against
// limits::components_per_message; throws ModuleError(code="too_many_components_v2",
// category=validation) when exceeded.
inline json components_v2_message(const json& components) {
    std::size_t total = 0;
    if (components.is_array()) {
        for (const auto& node : components) {
            total += detail::count_components_v2(node);
        }
    }
    if (total > limits::components_per_message) {
        throw MODULE_ERROR("too_many_components_v2", ErrorCategory::validation,
                           "components v2 message holds more than " +
                               std::to_string(limits::components_per_message) +
                               " components (" + std::to_string(total) + ")");
    }
    json result = json::object();
    result["flags"] = message_flag_components_v2;
    result["components"] = components;
    return result;
}

}  // namespace discord_interactions
