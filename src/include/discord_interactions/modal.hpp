#pragma once

// Modal interaction responses and modal-submit value extraction.
//
// Discord's CURRENT modal structure wraps each input in a Label component
// (type 18); the Label carries the human-readable label and optional
// description, and its "component" child is the bare input (e.g. a Text Input,
// type 4). The older "Action Row (type 1) containing a Text Input" modal layout
// is DEPRECATED and this header never emits it — text inputs are always returned
// bare (no "label" key of their own) so they can only be placed inside a Label
// wrapper via label_component().
//
// modal() returns the full {type:9, data:{...}} interaction response
// (type 9 = MODAL response callback). All strings clamp through the authoritative
// limits in limits.hpp, and every optional field is omitted when left at its
// default so payloads stay minimal.
//
// Header-only, no third-party runtime dependencies beyond nlohmann/json
// (AD-2/AD-7). Consumed identically by unit tests and Lambdas.

#include <nlohmann/json.hpp>

#include <initializer_list>
#include <optional>
#include <string>

#include "discord_interactions/errors.hpp"
#include "discord_interactions/limits.hpp"

namespace discord_interactions {

using json = nlohmann::json;

enum class TextInputStyle {
    short_input = 1,
    paragraph = 2,
};

// A bare Text Input (component type 4). It carries NO "label" key — the label
// lives on the enclosing Label component (see label_component). Optional fields
// (placeholder, value, min_length, max_length) are omitted when left at their
// defaults; max_length == 0 means "omit". custom_id clamps to limits::custom_id
// and value clamps to limits::text_input_value.
inline json text_input(const std::string& custom_id, TextInputStyle style,
                       bool required = true, const std::string& placeholder = "",
                       const std::string& value = "",
                       int min_length = 0, int max_length = 0) {
    json input = json::object();
    input["type"] = 4;
    input["custom_id"] = safe_truncate(custom_id, limits::custom_id);
    input["style"] = static_cast<int>(style);
    input["required"] = required;

    if (!placeholder.empty()) {
        input["placeholder"] = placeholder;
    }
    if (!value.empty()) {
        input["value"] = safe_truncate(value, limits::text_input_value);
    }
    if (min_length > 0) {
        input["min_length"] = min_length;
    }
    if (max_length > 0) {
        input["max_length"] = max_length;
    }
    return input;
}

// A Label component (type 18) — the current wrapper for a modal input. The
// label clamps to limits::text_input_label; the optional description is omitted
// when empty. The child input is stored under the singular "component" key.
inline json label_component(const std::string& label, const json& child,
                            const std::string& description = "") {
    json wrapper = json::object();
    wrapper["type"] = 18;
    wrapper["label"] = safe_truncate(label, limits::text_input_label);
    if (!description.empty()) {
        wrapper["description"] = description;
    }
    wrapper["component"] = child;
    return wrapper;
}

// Builds a full MODAL interaction response ({type:9, data:{...}}). Components
// must be modal-legal top-level types: Label (type 18) or Text Display
// (type 10). A bare Text Input (type 4) — or any other non-top-level type —
// throws ModuleError(validation), as does a component count exceeding
// limits::components_per_message (40). title clamps to limits::modal_title and
// custom_id to limits::custom_id.
inline json modal(const std::string& custom_id, const std::string& title,
                  std::initializer_list<json> components) {
    if (components.size() > limits::components_per_message) {
        throw MODULE_ERROR("modal_too_many_components", ErrorCategory::validation,
                           "modal exceeds the components-per-message limit");
    }

    json component_array = json::array();
    for (const auto& component : components) {
        int type = 0;
        if (component.is_object()) {
            const auto it = component.find("type");
            if (it != component.end() && it->is_number_integer()) {
                type = it->get<int>();
            }
        }
        // Only Label (18) and Text Display (10) are legal at the top level.
        // A bare Text Input (4) or a deprecated Action Row (1) is rejected.
        if (type != 18 && type != 10) {
            throw MODULE_ERROR("modal_invalid_component", ErrorCategory::validation,
                               "modal component must be a Label (18) or Text Display (10)");
        }
        component_array.push_back(component);
    }

    json data = json::object();
    data["custom_id"] = safe_truncate(custom_id, limits::custom_id);
    data["title"] = safe_truncate(title, limits::modal_title);
    data["components"] = std::move(component_array);

    json response = json::object();
    response["type"] = 9;
    response["data"] = std::move(data);
    return response;
}

namespace detail {

// Depth-first search for an object whose "custom_id" equals target and which
// carries a string "value"; recurses through Label wrappers ("component") and
// any nested "components"/array structures.
inline std::optional<std::string> find_modal_value(const json& node,
                                                   const std::string& custom_id) {
    if (node.is_object()) {
        const auto id_it = node.find("custom_id");
        const auto value_it = node.find("value");
        if (id_it != node.end() && id_it->is_string() &&
            id_it->get<std::string>() == custom_id && value_it != node.end() &&
            value_it->is_string()) {
            return value_it->get<std::string>();
        }
        for (const auto& child : node.items()) {
            auto found = find_modal_value(child.value(), custom_id);
            if (found.has_value()) {
                return found;
            }
        }
    } else if (node.is_array()) {
        for (const auto& child : node) {
            auto found = find_modal_value(child, custom_id);
            if (found.has_value()) {
                return found;
            }
        }
    }
    return std::nullopt;
}

}  // namespace detail

// Extracts a submitted text value from a MODAL_SUBMIT interaction by custom_id,
// recursing through Label wrappers. Returns nullopt when the input is absent or
// carries no string value.
inline std::optional<std::string> modal_value(const json& interaction,
                                              const std::string& custom_id) {
    if (!interaction.is_object()) {
        return std::nullopt;
    }
    const auto data_it = interaction.find("data");
    if (data_it == interaction.end() || !data_it->is_object()) {
        return std::nullopt;
    }
    const auto components_it = data_it->find("components");
    if (components_it == data_it->end()) {
        return std::nullopt;
    }
    return detail::find_modal_value(*components_it, custom_id);
}

}  // namespace discord_interactions
