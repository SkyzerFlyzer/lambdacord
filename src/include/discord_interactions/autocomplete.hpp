#pragma once

// Autocomplete response helpers for Discord APPLICATION_COMMAND_AUTOCOMPLETE
// interactions (type 4), the equivalent of discord.js's
// AutocompleteInteraction.respond().
//
// Discord accepts at most 25 autocomplete choices, each with a name <= 100
// characters. These helpers build individual {name, value} choice objects,
// assemble a safe {type:8, data:{choices:[...]}} response (capping at 25 and
// truncating names via limits::choice_name), filter a choice list against the
// user's partial input (case-insensitive, prefix matches before substring
// matches, stable within each class), and read the focused option's name/value
// out of the interaction — walking nested subcommand/group option lists.
//
// Header-only (AD-2), namespace discord_interactions. Depends only on
// nlohmann/json and limits.hpp — no AWS SDK, libsodium, or curl.

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

#include "discord_interactions/limits.hpp"

namespace discord_interactions {

using json = nlohmann::json;

// Discord application command option types used for autocomplete descent.
namespace autocomplete_detail {
inline constexpr int sub_command = 1;
inline constexpr int sub_command_group = 2;

// ASCII-lowercase copy for case-insensitive comparison. Autocomplete queries
// are short user input; a byte-wise lowercase is sufficient for the common
// ASCII case and never throws on multibyte input (bytes >= 0x80 are unchanged).
inline std::string to_lower_ascii(const std::string& s) {
    std::string out{};
    out.reserve(s.size());
    for (const char c : s) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc >= 'A' && uc <= 'Z') {
            out.push_back(static_cast<char>(uc - 'A' + 'a'));
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// Recursively search an option array for the option carrying "focused": true,
// descending through subcommand (1) and subcommand group (2) option lists.
// Returns a pointer to the focused option object, or nullptr when none is found.
inline const json* find_focused(const json& options) {
    if (!options.is_array()) {
        return nullptr;
    }
    for (const auto& option : options) {
        if (!option.is_object()) {
            continue;
        }
        if (option.value("focused", false)) {
            return &option;
        }
        const int type = option.value("type", 0);
        if (type == sub_command || type == sub_command_group) {
            if (option.contains("options")) {
                const json* nested = find_focused(option.at("options"));
                if (nested != nullptr) {
                    return nested;
                }
            }
        }
    }
    return nullptr;
}

// Returns a pointer to the focused option object within the interaction, or
// nullptr when there is no data/options or nothing is focused.
inline const json* focused_option(const json& interaction) {
    if (!interaction.is_object() || !interaction.contains("data")) {
        return nullptr;
    }
    const json& data = interaction.at("data");
    if (!data.is_object() || !data.contains("options")) {
        return nullptr;
    }
    return find_focused(data.at("options"));
}

}  // namespace autocomplete_detail

// Choice builders. The name is stored verbatim here; autocomplete_response is
// responsible for truncating names to limits::choice_name so a raw choice can
// be inspected exactly in tests and callers.
inline json choice(const std::string& name, const std::string& value) {
    return json{{"name", name}, {"value", value}};
}

inline json choice(const std::string& name, int64_t value) {
    return json{{"name", name}, {"value", value}};
}

inline json choice(const std::string& name, double value) {
    return json{{"name", name}, {"value", value}};
}

// Wraps a choices array into an autocomplete response: keeps only the first
// limits::autocomplete_choices (25) entries and truncates each choice name to
// limits::choice_name (100) via safe_truncate. Returns
// {type:8, data:{choices:[...]}}.
inline json autocomplete_response(const json& choices) {
    json out = json::array();
    if (choices.is_array()) {
        for (const auto& c : choices) {
            if (out.size() >= limits::autocomplete_choices) {
                break;
            }
            json entry = c;
            if (entry.is_object() && entry.contains("name") &&
                entry.at("name").is_string()) {
                entry["name"] =
                    safe_truncate(entry.at("name").get<std::string>(), limits::choice_name);
            }
            out.push_back(std::move(entry));
        }
    }
    return json{{"type", 8}, {"data", {{"choices", std::move(out)}}}};
}

// Case-insensitive filter over a {name, value} array. Prefix matches on the
// choice name are returned first, then substring matches; order within each
// class is stable (input order preserved). An empty query returns the input
// unchanged.
inline json filter_choices(const json& choices, const std::string& query) {
    if (!choices.is_array()) {
        return json::array();
    }
    if (query.empty()) {
        return choices;
    }

    const std::string needle = autocomplete_detail::to_lower_ascii(query);

    json prefix_matches = json::array();
    json substring_matches = json::array();
    for (const auto& c : choices) {
        if (!c.is_object() || !c.contains("name") || !c.at("name").is_string()) {
            continue;
        }
        const std::string name =
            autocomplete_detail::to_lower_ascii(c.at("name").get<std::string>());
        const std::string::size_type pos = name.find(needle);
        if (pos == 0) {
            prefix_matches.push_back(c);
        } else if (pos != std::string::npos) {
            substring_matches.push_back(c);
        }
    }

    json out = json::array();
    for (auto& c : prefix_matches) {
        out.push_back(std::move(c));
    }
    for (auto& c : substring_matches) {
        out.push_back(std::move(c));
    }
    return out;
}

// Name of the focused option, or nullopt when none is focused.
inline std::optional<std::string> focused_option_name(const json& interaction) {
    const json* option = autocomplete_detail::focused_option(interaction);
    if (option == nullptr || !option->contains("name") ||
        !option->at("name").is_string()) {
        return std::nullopt;
    }
    return option->at("name").get<std::string>();
}

// Value of the focused option as a string. Discord usually sends the partial
// input as a string; when the option is numeric the JSON value is stringified.
// Returns nullopt when nothing is focused or the value is absent.
inline std::optional<std::string> focused_option_value(const json& interaction) {
    const json* option = autocomplete_detail::focused_option(interaction);
    if (option == nullptr || !option->contains("value")) {
        return std::nullopt;
    }
    const json& value = option->at("value");
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<int64_t>());
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<uint64_t>());
    }
    if (value.is_number_float()) {
        return json(value).dump();
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? std::string("true") : std::string("false");
    }
    return std::nullopt;
}

}  // namespace discord_interactions
