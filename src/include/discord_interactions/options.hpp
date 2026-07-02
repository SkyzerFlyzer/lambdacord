#pragma once

// Typed command-option access for Discord CHAT_INPUT interactions.
//
// Discord delivers slash-command arguments as a nested list under
// `interaction.data.options`. Subcommands (type 1) and subcommand groups
// (type 2) wrap the real argument list one or two levels deep. These helpers
// walk to the innermost option list and read individual values with the caller's
// expected type, returning std::nullopt on a type mismatch or a missing option
// instead of throwing. Snowflake-valued options (user/channel/role/mentionable/
// attachment) are read as strings via option_id. Resolved-data lookups return
// the matching object from `interaction.data.resolved`, or an empty object when
// absent.

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace discord_interactions {

using json = nlohmann::json;

// Discord application command option types.
namespace option_type {
inline constexpr int sub_command = 1;
inline constexpr int sub_command_group = 2;
inline constexpr int string = 3;
inline constexpr int integer = 4;
inline constexpr int boolean = 5;
inline constexpr int user = 6;
inline constexpr int channel = 7;
inline constexpr int role = 8;
inline constexpr int mentionable = 9;
inline constexpr int number = 10;
inline constexpr int attachment = 11;
}  // namespace option_type

// Walks nested subcommand/group options to the innermost option list. Returns an
// empty array when `data` or `data.options` is absent.
inline json command_options(const json& interaction) {
    if (!interaction.is_object() || !interaction.contains("data")) {
        return json::array();
    }

    const json& data = interaction.at("data");
    if (!data.is_object()) {
        return json::array();
    }

    json options = data.value("options", json::array());
    if (!options.is_array()) {
        return json::array();
    }

    // Descend while the first option is a subcommand or subcommand group.
    while (!options.empty() && options.at(0).is_object()) {
        const int type = options.at(0).value("type", 0);
        if (type != option_type::sub_command && type != option_type::sub_command_group) {
            break;
        }

        json inner = options.at(0).value("options", json::array());
        if (!inner.is_array()) {
            return json::array();
        }
        options = inner;
    }

    return options;
}

namespace detail {

// Returns a copy of the named option object from the innermost option list, or an
// empty object if it is not present.
inline json find_option(const json& interaction, const std::string& name) {
    const json options = command_options(interaction);
    for (const auto& option : options) {
        if (option.is_object() && option.value("name", "") == name) {
            return option;
        }
    }
    return json::object();
}

// Returns the resolved-data entry for `id` under the given category, or an empty
// object when the interaction has no matching resolved entry.
inline json resolved_entry(const json& interaction, const std::string& category,
                           const std::string& id) {
    if (!interaction.is_object() || !interaction.contains("data")) {
        return json::object();
    }

    const json& data = interaction.at("data");
    if (!data.is_object() || !data.contains("resolved")) {
        return json::object();
    }

    const json& resolved = data.at("resolved");
    if (!resolved.is_object() || !resolved.contains(category)) {
        return json::object();
    }

    const json& bucket = resolved.at(category);
    if (!bucket.is_object() || !bucket.contains(id)) {
        return json::object();
    }

    const json& entry = bucket.at(id);
    return entry.is_object() ? entry : json::object();
}

}  // namespace detail

inline std::optional<std::string> option_string(const json& interaction,
                                                const std::string& name) {
    const json option = detail::find_option(interaction, name);
    if (option.contains("value") && option.at("value").is_string()) {
        return option.at("value").get<std::string>();
    }
    return std::nullopt;
}

inline std::optional<int64_t> option_int(const json& interaction,
                                         const std::string& name) {
    const json option = detail::find_option(interaction, name);
    // is_number_integer() excludes floating-point values, so a NUMBER option
    // holding 3.5 is a mismatch rather than a silent truncation.
    if (option.contains("value") && option.at("value").is_number_integer()) {
        return option.at("value").get<int64_t>();
    }
    return std::nullopt;
}

inline std::optional<double> option_number(const json& interaction,
                                           const std::string& name) {
    const json option = detail::find_option(interaction, name);
    // Accept any JSON number (integer or float); a boolean is not a number.
    if (option.contains("value") && option.at("value").is_number()) {
        return option.at("value").get<double>();
    }
    return std::nullopt;
}

inline std::optional<bool> option_bool(const json& interaction,
                                       const std::string& name) {
    const json option = detail::find_option(interaction, name);
    if (option.contains("value") && option.at("value").is_boolean()) {
        return option.at("value").get<bool>();
    }
    return std::nullopt;
}

// Snowflake-valued options: user (6), channel (7), role (8), mentionable (9),
// attachment (11). The option's declared type must be one of these — a plain
// STRING option whose value happens to be numeric is not treated as an id.
inline std::optional<std::string> option_id(const json& interaction,
                                            const std::string& name) {
    const json option = detail::find_option(interaction, name);
    const int type = option.value("type", 0);
    const bool is_snowflake_type =
        type == option_type::user || type == option_type::channel ||
        type == option_type::role || type == option_type::mentionable ||
        type == option_type::attachment;
    if (is_snowflake_type && option.contains("value") && option.at("value").is_string()) {
        return option.at("value").get<std::string>();
    }
    return std::nullopt;
}

inline json resolved_user(const json& interaction, const std::string& user_id) {
    return detail::resolved_entry(interaction, "users", user_id);
}

inline json resolved_member(const json& interaction, const std::string& user_id) {
    return detail::resolved_entry(interaction, "members", user_id);
}

inline json resolved_channel(const json& interaction, const std::string& channel_id) {
    return detail::resolved_entry(interaction, "channels", channel_id);
}

inline json resolved_role(const json& interaction, const std::string& role_id) {
    return detail::resolved_entry(interaction, "roles", role_id);
}

inline json resolved_attachment(const json& interaction, const std::string& attachment_id) {
    return detail::resolved_entry(interaction, "attachments", attachment_id);
}

}  // namespace discord_interactions
