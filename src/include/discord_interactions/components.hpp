#pragma once

// Message component (v1) builders: buttons (all styles), string select, and the
// user/role/channel entity selects, plus action-row composition with Discord's
// documented row limits.
//
// These factories mirror discord.js's ActionRowBuilder / *ButtonBuilder /
// *SelectMenuBuilder surface within this framework's stateless model. They emit
// plain nlohmann::json so unit tests and Lambdas consume them identically
// (AD-2). Header-only, standard library plus nlohmann/json and the framework's
// limits.hpp / errors.hpp / json_access.hpp only (AD-7). custom_ids and select
// values are machine-facing and clamp through limits.hpp's clamp_utf8 (never
// the ellipsis-appending safe_truncate).
//
// Component type codes emitted here follow Discord's reference: action row 1,
// button 2, string select 3, user select 5, role select 6, channel select 8.
// The mentionable select (type 7) is intentionally omitted per the T1.4 spec;
// user/role/channel selects cover the entity-select surface this framework uses.

#include <nlohmann/json.hpp>

#include <initializer_list>
#include <string>

#include "discord_interactions/errors.hpp"
#include "discord_interactions/json_access.hpp"
#include "discord_interactions/limits.hpp"

namespace discord_interactions {

using json = nlohmann::json;

enum class ButtonStyle {
    primary = 1,
    secondary = 2,
    success = 3,
    danger = 4,
    link = 5,
};

// A button (component type 2). ButtonStyle::link emits `url` and no `custom_id`;
// every other style emits `custom_id`, clamped to limits::custom_id via
// clamp_utf8 (machine-facing, so no ellipsis is ever injected). `disabled` and
// `emoji` are emitted only when non-default (disabled == true, emoji non-null).
inline json button(ButtonStyle style, const std::string& custom_id_or_url,
                   const std::string& label, bool disabled = false,
                   const json& emoji = json()) {
    json result = json::object();
    result["type"] = 2;
    result["style"] = static_cast<int>(style);
    result["label"] = safe_truncate(label, limits::button_label);
    if (style == ButtonStyle::link) {
        result["url"] = custom_id_or_url;
    } else {
        result["custom_id"] = clamp_utf8(custom_id_or_url, limits::custom_id);
    }
    if (disabled) {
        result["disabled"] = true;
    }
    if (!emoji.is_null()) {
        result["emoji"] = emoji;
    }
    return result;
}

// A single option for a string select (used inside `string_select`). `label`,
// `value`, and `description` each clamp to limits::select_option_field (100).
// `label` and `description` clamp with safe_truncate (appending an ellipsis);
// `value` is machine-facing (echoed back verbatim on selection) so it clamps
// WITHOUT an ellipsis. `description` and `default` are emitted only when
// non-default.
inline json select_option(const std::string& label, const std::string& value,
                          const std::string& description = "",
                          bool is_default = false) {
    json result = json::object();
    result["label"] = safe_truncate(label, limits::select_option_field);
    result["value"] = clamp_utf8(value, limits::select_option_field);
    if (!description.empty()) {
        result["description"] = safe_truncate(description, limits::select_option_field);
    }
    if (is_default) {
        result["default"] = true;
    }
    return result;
}

// A string select menu (component type 3). `placeholder`/`min_values`/
// `max_values`/`disabled` are emitted only when non-default (min/max default 1).
// Throws ModuleError(code="too_many_select_options", category=validation) when
// `options` holds more than limits::select_options (25) entries, and
// ModuleError(code="invalid_select_values", category=validation) when the
// documented min/max range is violated: min_values and max_values must each be
// 0..limits::select_values_max (25), min_values <= max_values, and max_values
// must not exceed the option count when `options` is non-empty.
inline json string_select(const std::string& custom_id, const json& options,
                          const std::string& placeholder = "", int min_values = 1,
                          int max_values = 1, bool disabled = false) {
    if (options.is_array() && options.size() > limits::select_options) {
        throw MODULE_ERROR("too_many_select_options", ErrorCategory::validation,
                           "string select holds more than " +
                               std::to_string(limits::select_options) + " options");
    }
    const int values_cap = static_cast<int>(limits::select_values_max);
    if (min_values < 0 || min_values > values_cap || max_values < 0 ||
        max_values > values_cap || min_values > max_values) {
        throw MODULE_ERROR("invalid_select_values", ErrorCategory::validation,
                           "string select min/max_values must satisfy 0 <= min <= max <= " +
                               std::to_string(values_cap));
    }
    if (options.is_array() && !options.empty() &&
        static_cast<std::size_t>(max_values) > options.size()) {
        throw MODULE_ERROR("invalid_select_values", ErrorCategory::validation,
                           "string select max_values exceeds the option count");
    }
    json result = json::object();
    result["type"] = 3;
    result["custom_id"] = clamp_utf8(custom_id, limits::custom_id);
    result["options"] = options;
    if (!placeholder.empty()) {
        result["placeholder"] = placeholder;
    }
    if (min_values != 1) {
        result["min_values"] = min_values;
    }
    if (max_values != 1) {
        result["max_values"] = max_values;
    }
    if (disabled) {
        result["disabled"] = true;
    }
    return result;
}

namespace detail {

// Shared body for the entity selects (user/role/channel) that differ only by
// their type code.
inline json entity_select(int type, const std::string& custom_id,
                          const std::string& placeholder) {
    json result = json::object();
    result["type"] = type;
    result["custom_id"] = clamp_utf8(custom_id, limits::custom_id);
    if (!placeholder.empty()) {
        result["placeholder"] = placeholder;
    }
    return result;
}

}  // namespace detail

// A user select menu (component type 5).
inline json user_select(const std::string& custom_id,
                        const std::string& placeholder = "") {
    return detail::entity_select(5, custom_id, placeholder);
}

// A role select menu (component type 6).
inline json role_select(const std::string& custom_id,
                        const std::string& placeholder = "") {
    return detail::entity_select(6, custom_id, placeholder);
}

// A channel select menu (component type 8). `channel_types` is emitted only when
// non-empty (defaults to an empty array).
inline json channel_select(const std::string& custom_id,
                           const std::string& placeholder = "",
                           const json& channel_types = json::array()) {
    json result = detail::entity_select(8, custom_id, placeholder);
    if (!channel_types.empty()) {
        result["channel_types"] = channel_types;
    }
    return result;
}

// Wraps components in an action row (component type 1). A Discord action row may
// hold EITHER up to limits::buttons_per_row buttons OR exactly one select menu,
// never a mix of the two. Throws
// ModuleError(code="too_many_components", category=validation) past those counts,
// and ModuleError(code="mixed_row_components", category=validation) when the row
// mixes at least one select with at least one button.
inline json action_row(const json& components) {
    std::size_t buttons = 0;
    std::size_t selects = 0;
    for (const auto& component : components) {
        // Throw-free read (json_access.hpp): foreign component JSON with a
        // wrong-typed "type" counts as neither a button nor a select.
        const int type = get_if<int>(component, "type").value_or(0);
        if (type == 2) {
            ++buttons;
        } else if (type == 3 || type == 5 || type == 6 || type == 7 || type == 8) {
            ++selects;
        }
    }
    if (buttons > limits::buttons_per_row) {
        throw MODULE_ERROR("too_many_components", ErrorCategory::validation,
                           "action row holds more than " +
                               std::to_string(limits::buttons_per_row) + " buttons");
    }
    if (selects > 1) {
        throw MODULE_ERROR("too_many_components", ErrorCategory::validation,
                           "action row holds more than one select menu");
    }
    if (selects >= 1 && buttons >= 1) {
        throw MODULE_ERROR("mixed_row_components", ErrorCategory::validation,
                           "action row mixes a select menu with buttons");
    }
    json result = json::object();
    result["type"] = 1;
    result["components"] = components;
    return result;
}

// Assembles action rows into a component array. Throws
// ModuleError(code="too_many_components", category=validation) past
// limits::action_rows rows.
inline json rows(std::initializer_list<json> row_list) {
    if (row_list.size() > limits::action_rows) {
        throw MODULE_ERROR("too_many_components", ErrorCategory::validation,
                           "message holds more than " +
                               std::to_string(limits::action_rows) + " action rows");
    }
    json result = json::array();
    for (const auto& row : row_list) {
        result.push_back(row);
    }
    return result;
}

}  // namespace discord_interactions
