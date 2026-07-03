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
#include <vector>

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
// via clamp_utf8 (machine-facing, no ellipsis) and value clamps to
// limits::text_input_value. Documented length ranges are enforced: min_length
// must be 0..limits::text_input_value (4000), a set max_length must be
// 1..limits::text_input_value, and min_length <= max_length when both are set;
// violations throw ModuleError(code="invalid_text_input_length", validation).
inline json text_input(const std::string& custom_id, TextInputStyle style,
                       bool required = true, const std::string& placeholder = "",
                       const std::string& value = "",
                       int min_length = 0, int max_length = 0) {
    const int length_cap = static_cast<int>(limits::text_input_value);
    if (min_length < 0 || min_length > length_cap || max_length < 0 ||
        max_length > length_cap ||
        (max_length != 0 && min_length > max_length)) {
        throw MODULE_ERROR("invalid_text_input_length", ErrorCategory::validation,
                           "text input requires min_length 0.." +
                               std::to_string(length_cap) + ", max_length 1.." +
                               std::to_string(length_cap) +
                               " when set, and min_length <= max_length");
    }
    json input = json::object();
    input["type"] = 4;
    input["custom_id"] = clamp_utf8(custom_id, limits::custom_id);
    input["style"] = static_cast<int>(style);
    input["required"] = required;

    if (!placeholder.empty()) {
        input["placeholder"] = safe_truncate(placeholder, limits::text_input_placeholder);
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
// label clamps to limits::text_input_label and the optional description clamps
// to limits::label_description (omitted when empty). The child input is stored
// under the singular "component" key and MUST be a modal-legal input component:
// String/User/Role/Mentionable/Channel Select (3/5/6/7/8), Text Input (4),
// File Upload (19), Radio Group (21), Checkbox Group (22), or Checkbox (23).
// Anything else — e.g. a Button (type 2) or a deprecated Action Row (type 1) —
// throws ModuleError(code="invalid_label_child", validation).
inline json label_component(const std::string& label, const json& child,
                            const std::string& description = "") {
    int child_type = 0;
    if (child.is_object()) {
        const auto it = child.find("type");
        if (it != child.end() && it->is_number_integer()) {
            child_type = it->get<int>();
        }
    }
    switch (child_type) {
        case 3:   // String Select
        case 4:   // Text Input
        case 5:   // User Select
        case 6:   // Role Select
        case 7:   // Mentionable Select
        case 8:   // Channel Select
        case 19:  // File Upload
        case 21:  // Radio Group
        case 22:  // Checkbox Group
        case 23:  // Checkbox
            break;
        default:
            throw MODULE_ERROR("invalid_label_child", ErrorCategory::validation,
                               "label component child must be a modal-legal input component");
    }

    json wrapper = json::object();
    wrapper["type"] = 18;
    wrapper["label"] = safe_truncate(label, limits::text_input_label);
    if (!description.empty()) {
        wrapper["description"] = safe_truncate(description, limits::label_description);
    }
    wrapper["component"] = child;
    return wrapper;
}

// ---------------------------------------------------------------------------
// Additional modal input components. Per the Discord component reference all of
// these are bare inputs that MUST be placed inside a Label wrapper (type 18) via
// label_component(); modal() therefore still only accepts Label / Text Display
// at the top level. custom_ids clamp to limits::custom_id, and every optional
// field is omitted when left at its default so payloads stay minimal.
// ---------------------------------------------------------------------------

// File Upload (component type 19). On MODAL_SUBMIT the uploaded attachment
// snowflakes arrive as a "values" array — the same key a Checkbox Group uses
// (extract with modal_values).
inline json file_upload(const std::string& custom_id, bool required = true) {
    json input = json::object();
    input["type"] = 19;
    input["custom_id"] = clamp_utf8(custom_id, limits::custom_id);
    input["required"] = required;
    return input;
}

// A single option for a Radio Group or Checkbox Group. label/value/description
// each clamp to Discord's 100-char option limit; description is omitted when
// empty and the "default" key is emitted only when is_default is true.
inline json radio_option(const std::string& label, const std::string& value,
                         const std::string& description = "",
                         bool is_default = false) {
    json option = json::object();
    option["label"] = safe_truncate(label, limits::choice_name);
    // Values round-trip through modal submits — machine-facing, never ellipsized.
    option["value"] = clamp_utf8(value, limits::choice_name);
    if (!description.empty()) {
        option["description"] = safe_truncate(description, limits::choice_name);
    }
    if (is_default) {
        option["default"] = true;
    }
    return option;
}

// Radio Group (component type 21) — a single-choice list. On MODAL_SUBMIT the
// chosen option's value arrives as a string "value". A radio group must offer
// between limits::radio_options_min (2) and limits::radio_options_max (10)
// options; anything else throws ModuleError(code="invalid_radio_options",
// validation).
inline json radio_group(const std::string& custom_id, const json& options,
                        bool required = true) {
    if (!options.is_array() || options.size() < limits::radio_options_min ||
        options.size() > limits::radio_options_max) {
        throw MODULE_ERROR("invalid_radio_options", ErrorCategory::validation,
                           "radio group must have between " +
                               std::to_string(limits::radio_options_min) + " and " +
                               std::to_string(limits::radio_options_max) + " options");
    }
    json input = json::object();
    input["type"] = 21;
    input["custom_id"] = clamp_utf8(custom_id, limits::custom_id);
    input["options"] = options;
    input["required"] = required;
    return input;
}

// Checkbox Group (component type 22) — a multi-choice list. On MODAL_SUBMIT the
// selected option values arrive as a "values" array (extract with modal_values).
// `options` must be an array of limits::checkbox_group_options_min (1) to
// limits::checkbox_group_options_max (10) entries; a non-array, empty, or
// oversized list throws ModuleError(code="invalid_checkbox_options", validation).
// min_values / max_values are ADDITIVE optional fields; -1 (the default) means
// "omit". When either is set (!= -1) it must satisfy
// 0 <= min <= max <= limits::checkbox_group_max_values (10), a set max_values
// must additionally be >= 1, and when required == true a set min_values must be
// >= 1 (a required group cannot allow zero selections). Violations throw
// ModuleError(code="invalid_checkbox_values", validation).
inline json checkbox_group(const std::string& custom_id, const json& options,
                           bool required = true, int min_values = -1,
                           int max_values = -1) {
    if (!options.is_array() ||
        options.size() < limits::checkbox_group_options_min ||
        options.size() > limits::checkbox_group_options_max) {
        throw MODULE_ERROR("invalid_checkbox_options", ErrorCategory::validation,
                           "checkbox group must have between " +
                               std::to_string(limits::checkbox_group_options_min) +
                               " and " +
                               std::to_string(limits::checkbox_group_options_max) +
                               " options");
    }
    const int max_cap = static_cast<int>(limits::checkbox_group_max_values);
    const bool min_set = min_values != -1;
    const bool max_set = max_values != -1;
    if ((min_set && (min_values < 0 || min_values > max_cap)) ||
        (max_set && (max_values < 1 || max_values > max_cap)) ||
        (min_set && max_set && min_values > max_values) ||
        (required && min_set && min_values < 1)) {
        throw MODULE_ERROR("invalid_checkbox_values", ErrorCategory::validation,
                           "checkbox group min/max_values must satisfy 0 <= min <= max <= " +
                               std::to_string(max_cap) +
                               ", with a set max_values >= 1 and a set min_values >= 1 "
                               "when the group is required");
    }
    json input = json::object();
    input["type"] = 22;
    input["custom_id"] = clamp_utf8(custom_id, limits::custom_id);
    input["options"] = options;
    input["required"] = required;
    if (min_set) {
        input["min_values"] = min_values;
    }
    if (max_set) {
        input["max_values"] = max_values;
    }
    return input;
}

// Checkbox (component type 23) — a single boolean checkbox. Its visible text is
// supplied by the enclosing Label wrapper (pair with label_component), so the
// input carries no "label" of its own. "default" (the initial checked state) is
// an ADDITIVE optional field from the component reference, emitted only when
// is_default is true. On MODAL_SUBMIT the state arrives as a boolean "value"
// (extract with modal_checked).
//
// NOTE: the `required` parameter is retained for source compatibility but is
// deliberately IGNORED — this factory never emits a "required" field. Discord's
// component reference does not document a "required" field on a standalone
// Checkbox, so omitting it is safe either way: if the field exists, Discord's
// documented default applies; if it does not, we avoid sending an invalid field.
inline json checkbox(const std::string& custom_id, bool required = false,
                     bool is_default = false) {
    (void)required;  // intentionally unused; see the NOTE above.
    json input = json::object();
    input["type"] = 23;
    input["custom_id"] = clamp_utf8(custom_id, limits::custom_id);
    if (is_default) {
        input["default"] = true;
    }
    return input;
}

// Builds a full MODAL interaction response ({type:9, data:{...}}). Components
// must be modal-legal top-level types: Label (type 18) or Text Display
// (type 10). A bare Text Input (type 4) — or any other non-top-level type —
// throws ModuleError(validation). A modal callback carries between 1 and
// limits::modal_components (5) top-level components (Discord's modal cap, which
// is distinct from the 40-component message cap); an empty list or more than 5
// throws ModuleError(validation). title clamps to limits::modal_title and
// custom_id to limits::custom_id.
inline json modal(const std::string& custom_id, const std::string& title,
                  std::initializer_list<json> components) {
    if (components.size() < 1) {
        throw MODULE_ERROR("modal_no_components", ErrorCategory::validation,
                           "modal must have at least one component");
    }
    if (components.size() > limits::modal_components) {
        throw MODULE_ERROR("modal_too_many_components", ErrorCategory::validation,
                           "modal exceeds the " +
                               std::to_string(limits::modal_components) +
                               "-component modal limit");
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
    data["custom_id"] = clamp_utf8(custom_id, limits::custom_id);
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

// Depth-first collection of the string elements of a submitted component's
// "values" array — the documented multi-value submit key for both a Checkbox
// Group (selected option values) and a File Upload (attachment snowflakes).
// Recurses through Label wrappers and nested arrays; stops at the first object
// whose "custom_id" matches (custom_ids are unique within a modal).
inline void collect_modal_values(const json& node, const std::string& custom_id,
                                 std::vector<std::string>& out) {
    if (node.is_object()) {
        const auto id_it = node.find("custom_id");
        if (id_it != node.end() && id_it->is_string() &&
            id_it->get<std::string>() == custom_id) {
            const auto arr_it = node.find("values");
            if (arr_it != node.end() && arr_it->is_array()) {
                for (const auto& element : *arr_it) {
                    if (element.is_string()) {
                        out.push_back(element.get<std::string>());
                    }
                }
            }
            return;
        }
        for (const auto& child : node.items()) {
            collect_modal_values(child.value(), custom_id, out);
        }
    } else if (node.is_array()) {
        for (const auto& child : node) {
            collect_modal_values(child, custom_id, out);
        }
    }
}

// Depth-first search for an object whose "custom_id" equals target and which
// carries a boolean "value" (a Checkbox). Recurses through Label wrappers.
inline std::optional<bool> find_modal_checked(const json& node,
                                              const std::string& custom_id) {
    if (node.is_object()) {
        const auto id_it = node.find("custom_id");
        const auto value_it = node.find("value");
        if (id_it != node.end() && id_it->is_string() &&
            id_it->get<std::string>() == custom_id && value_it != node.end() &&
            value_it->is_boolean()) {
            return value_it->get<bool>();
        }
        for (const auto& child : node.items()) {
            auto found = find_modal_checked(child.value(), custom_id);
            if (found.has_value()) {
                return found;
            }
        }
    } else if (node.is_array()) {
        for (const auto& child : node) {
            auto found = find_modal_checked(child, custom_id);
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

// Extracts a submitted multi-value field from a MODAL_SUBMIT interaction by
// custom_id, recursing through Label wrappers. Both a Checkbox Group (selected
// option values) and a File Upload (uploaded attachment snowflakes) submit
// under the documented "values" key. Returns an empty vector when the input is
// absent or has no such array.
inline std::vector<std::string> modal_values(const json& interaction,
                                             const std::string& custom_id) {
    std::vector<std::string> out{};
    if (!interaction.is_object()) {
        return out;
    }
    const auto data_it = interaction.find("data");
    if (data_it == interaction.end() || !data_it->is_object()) {
        return out;
    }
    const auto components_it = data_it->find("components");
    if (components_it == data_it->end()) {
        return out;
    }
    detail::collect_modal_values(*components_it, custom_id, out);
    return out;
}

// Extracts a submitted Checkbox state from a MODAL_SUBMIT interaction by
// custom_id, recursing through Label wrappers. Returns nullopt when the checkbox
// is absent or carries no boolean value.
inline std::optional<bool> modal_checked(const json& interaction,
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
    return detail::find_modal_checked(*components_it, custom_id);
}

}  // namespace discord_interactions
