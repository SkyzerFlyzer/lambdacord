#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <string>

namespace discord_interactions {

using json = nlohmann::json;

struct InteractionMetadata {
    std::string application_id{};
    std::string token{};
    std::string guild_id{};
    std::string channel_id{};
    std::string user_id{};
};

inline std::string header_value(const json& headers, const std::string& key) {
    if (!headers.is_object()) {
        return "";
    }

    const auto exact = headers.find(key);
    if (exact != headers.end() && exact->is_string()) {
        return exact->get<std::string>();
    }

    std::string lowered_key = key;
    std::transform(lowered_key.begin(), lowered_key.end(), lowered_key.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

    for (auto it = headers.begin(); it != headers.end(); ++it) {
        if (!it.value().is_string()) {
            continue;
        }

        std::string candidate = it.key();
        std::transform(candidate.begin(), candidate.end(), candidate.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (candidate == lowered_key) {
            return it.value().get<std::string>();
        }
    }

    return "";
}

inline std::string interaction_user_id(const json& interaction) {
    const std::string member_user_id = interaction.value("member", json::object())
                                           .value("user", json::object())
                                           .value("id", "");
    if (!member_user_id.empty()) {
        return member_user_id;
    }

    return interaction.value("user", json::object()).value("id", "");
}

inline InteractionMetadata metadata(const json& interaction) {
    return {
        interaction.value("application_id", ""),
        interaction.value("token", ""),
        interaction.value("guild_id", ""),
        interaction.value("channel_id", ""),
        interaction_user_id(interaction),
    };
}

inline void append_selected_command_path(const json& options, std::ostringstream& full_name) {
    if (!options.is_array()) {
        return;
    }

    for (const auto& option : options) {
        const int type = option.value("type", 0);
        if (type != 1 && type != 2) {
            continue;
        }

        const std::string name = option.value("name", "");
        if (name.empty()) {
            return;
        }

        full_name << ' ' << name;
        append_selected_command_path(option.value("options", json::array()), full_name);
        return;
    }
}

inline std::string application_command_path(const json& interaction) {
    const json& data = interaction.at("data");
    std::ostringstream name{};
    name << data.value("name", "");
    append_selected_command_path(data.value("options", json::array()), name);
    return name.str();
}

inline std::string component_prefix(const std::string& custom_id) {
    if (custom_id.empty()) {
        throw std::runtime_error("message component interaction is missing custom_id");
    }

    const auto delimiter = custom_id.find(':');
    const std::string prefix =
        delimiter == std::string::npos ? custom_id : custom_id.substr(0, delimiter);
    if (prefix.empty()) {
        throw std::runtime_error("message component interaction custom_id prefix is empty");
    }
    return prefix;
}

inline std::string route_suffix_from_path(const std::string& path) {
    std::string suffix{};
    suffix.reserve(path.size());
    std::transform(path.begin(), path.end(), std::back_inserter(suffix),
                   [](char ch) { return ch == ' ' ? '-' : ch; });
    return suffix;
}

}  // namespace discord_interactions
