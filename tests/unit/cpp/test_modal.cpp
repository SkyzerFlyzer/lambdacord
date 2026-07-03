#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "discord_interactions/modal.hpp"

#include <optional>
#include <string>

using nlohmann::json;
using discord_interactions::TextInputStyle;
using discord_interactions::text_input;
using discord_interactions::label_component;
using discord_interactions::modal;
using discord_interactions::modal_value;
using discord_interactions::ModuleError;

namespace {

// Recursively reports whether any object in the tree carries "type": <code>.
// Used to prove a modal never emits a deprecated Action Row (type 1).
bool contains_type(const json& node, int type_code) {
    if (node.is_object()) {
        const auto it = node.find("type");
        if (it != node.end() && it->is_number_integer() && it->get<int>() == type_code) {
            return true;
        }
        for (const auto& child : node.items()) {
            if (contains_type(child.value(), type_code)) {
                return true;
            }
        }
    } else if (node.is_array()) {
        for (const auto& child : node) {
            if (contains_type(child, type_code)) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace

TEST_CASE("text_input short style emits type 4, style 1, no label key") {
    const json input = text_input("feedback", TextInputStyle::short_input);
    CHECK(input["type"] == 4);
    CHECK(input["style"] == 1);
    CHECK(input["custom_id"] == "feedback");
    CHECK(input["required"] == true);
    // Labels live on the Label wrapper, never on the input itself.
    CHECK_FALSE(input.contains("label"));
}

TEST_CASE("text_input paragraph style emits style 2") {
    const json input = text_input("body", TextInputStyle::paragraph);
    CHECK(input["type"] == 4);
    CHECK(input["style"] == 2);
}

TEST_CASE("text_input omits optional fields when defaulted") {
    const json input = text_input("id", TextInputStyle::short_input);
    CHECK_FALSE(input.contains("placeholder"));
    CHECK_FALSE(input.contains("value"));
    CHECK_FALSE(input.contains("min_length"));
    CHECK_FALSE(input.contains("max_length"));
}

TEST_CASE("text_input emits optional fields when provided") {
    const json input = text_input("id", TextInputStyle::paragraph, false,
                                  "type here", "prefill", 3, 200);
    CHECK(input["required"] == false);
    CHECK(input["placeholder"] == "type here");
    CHECK(input["value"] == "prefill");
    CHECK(input["min_length"] == 3);
    CHECK(input["max_length"] == 200);
}

TEST_CASE("text_input clamps custom_id to limits::custom_id") {
    const std::string long_id(150, 'a');
    const json input = text_input(long_id, TextInputStyle::short_input);
    CHECK(input["custom_id"].get<std::string>().size() <= 100u);
}

TEST_CASE("text_input clamps value to limits::text_input_value") {
    const std::string long_value(5000, 'x');
    const json input =
        text_input("id", TextInputStyle::paragraph, true, "", long_value);
    CHECK(input["value"].get<std::string>().size() <= 4000u);
}

TEST_CASE("label_component wraps a child with label and component key") {
    const json child = text_input("feedback", TextInputStyle::short_input);
    const json label = label_component("Your feedback", child);
    CHECK(label["type"] == 18);
    CHECK(label["label"] == "Your feedback");
    CHECK(label["component"] == child);
    CHECK_FALSE(label.contains("description"));
}

TEST_CASE("label_component emits description when provided") {
    const json child = text_input("feedback", TextInputStyle::short_input);
    const json label = label_component("Feedback", child, "Tell us more");
    CHECK(label["description"] == "Tell us more");
}

TEST_CASE("label_component clamps label to limits::text_input_label") {
    const std::string long_label(100, 'L');
    const json child = text_input("id", TextInputStyle::short_input);
    const json label = label_component(long_label, child);
    CHECK(label["label"].get<std::string>().size() <= 45u);
}

TEST_CASE("modal returns a type 9 response with custom_id, title and components") {
    const json child = text_input("feedback", TextInputStyle::short_input);
    const json response = modal("feedback_modal", "Send Feedback",
                                {label_component("Feedback", child)});
    CHECK(response["type"] == 9);
    CHECK(response["data"]["custom_id"] == "feedback_modal");
    CHECK(response["data"]["title"] == "Send Feedback");
    REQUIRE(response["data"]["components"].is_array());
    CHECK(response["data"]["components"].size() == 1);
    CHECK(response["data"]["components"][0]["type"] == 18);
}

TEST_CASE("modal accepts a Text Display (type 10) top-level component") {
    const json text_display = json{{"type", 10}, {"content", "Please fill this in"}};
    const json child = text_input("id", TextInputStyle::short_input);
    const json response =
        modal("m", "Title", {text_display, label_component("L", child)});
    CHECK(response["data"]["components"].size() == 2);
    CHECK(response["data"]["components"][0]["type"] == 10);
}

TEST_CASE("modal clamps title to limits::modal_title") {
    const std::string long_title(100, 'T');
    const json child = text_input("id", TextInputStyle::short_input);
    const json response = modal("m", long_title, {label_component("L", child)});
    CHECK(response["data"]["title"].get<std::string>().size() <= 45u);
}

TEST_CASE("modal clamps custom_id to limits::custom_id") {
    const std::string long_id(150, 'c');
    const json child = text_input("id", TextInputStyle::short_input);
    const json response = modal(long_id, "Title", {label_component("L", child)});
    CHECK(response["data"]["custom_id"].get<std::string>().size() <= 100u);
}

TEST_CASE("modal rejects a bare text input passed directly") {
    const json bare = text_input("feedback", TextInputStyle::short_input);
    CHECK_THROWS_AS(modal("m", "T", {bare}), ModuleError);
    try {
        modal("m", "T", {bare});
    } catch (const ModuleError& e) {
        CHECK(e.category == discord_interactions::ErrorCategory::validation);
    }
}

TEST_CASE("modal rejects more than 5 components") {
    const json label = label_component("L", text_input("a", TextInputStyle::short_input));
    // 6 Label components exceeds limits::modal_components (5) — modals allow 1..5.
    CHECK_THROWS_AS(modal("m", "T", {label, label, label, label, label, label}),
                    ModuleError);
    // Exactly 5 is legal.
    CHECK_NOTHROW(modal("m", "T", {label, label, label, label, label}));
}

TEST_CASE("modal rejects an empty component list (needs at least 1)") {
    CHECK_THROWS_AS(modal("m", "T", {}), ModuleError);
    try {
        modal("m", "T", {});
    } catch (const ModuleError& e) {
        CHECK(e.category == discord_interactions::ErrorCategory::validation);
    }
}

TEST_CASE("label_component clamps description to limits::label_description") {
    const std::string long_desc(200, 'D');
    const json child = text_input("id", TextInputStyle::short_input);
    const json label = label_component("L", child, long_desc);
    CHECK(label["description"].get<std::string>().size() <= 100u);
}

TEST_CASE("label_component rejects a non-input child (button type 2)") {
    const json btn = json{{"type", 2}, {"style", 1}, {"label", "Click"},
                          {"custom_id", "x"}};
    CHECK_THROWS_AS(label_component("L", btn), ModuleError);
    try {
        label_component("L", btn);
    } catch (const ModuleError& e) {
        CHECK(e.code == "invalid_label_child");
        CHECK(e.category == discord_interactions::ErrorCategory::validation);
    }
}

TEST_CASE("label_component rejects a deprecated action row child (type 1)") {
    const json row = json{{"type", 1}, {"components", json::array()}};
    CHECK_THROWS_AS(label_component("L", row), ModuleError);
}

TEST_CASE("label_component accepts a select-menu child (type 3)") {
    const json select = json{{"type", 3}, {"custom_id", "pick"},
                             {"options", json::array()}};
    CHECK_NOTHROW(label_component("L", select));
}

TEST_CASE("text_input clamps placeholder to limits::text_input_placeholder") {
    const std::string long_placeholder(200, 'p');
    const json input = text_input("id", TextInputStyle::short_input, true,
                                  long_placeholder);
    CHECK(input["placeholder"].get<std::string>().size() <= 100u);
}

TEST_CASE("modal emits no deprecated type-1 action rows anywhere") {
    const json child = text_input("feedback", TextInputStyle::paragraph, true,
                                  "placeholder", "value", 1, 100);
    const json response = modal("m", "Title",
                                {label_component("Feedback", child, "desc")});
    CHECK_FALSE(contains_type(response, 1));
    // Sanity: the Label (18) and text input (4) are present.
    CHECK(contains_type(response, 18));
    CHECK(contains_type(response, 4));
}

TEST_CASE("modal_value finds a value nested under a Label wrapper") {
    const json interaction = json{
        {"type", 5},
        {"data", {
            {"custom_id", "feedback_modal"},
            {"components", json::array({
                json{{"type", 18}, {"component", {
                    {"type", 4}, {"custom_id", "feedback"}, {"value", "great job"}}}},
            })},
        }},
    };
    const std::optional<std::string> value = modal_value(interaction, "feedback");
    REQUIRE(value.has_value());
    CHECK(value.value() == "great job");
}

TEST_CASE("modal_value returns nullopt when custom_id absent") {
    const json interaction = json{
        {"type", 5},
        {"data", {
            {"custom_id", "feedback_modal"},
            {"components", json::array({
                json{{"type", 18}, {"component", {
                    {"type", 4}, {"custom_id", "feedback"}, {"value", "hi"}}}},
            })},
        }},
    };
    CHECK_FALSE(modal_value(interaction, "missing").has_value());
    // Missing data entirely is handled too.
    CHECK_FALSE(modal_value(json{{"type", 5}}, "feedback").has_value());
}

TEST_CASE("modal_value extracts from a real-shaped multi-input MODAL_SUBMIT") {
    const json interaction = json{
        {"type", 5},
        {"data", {
            {"custom_id", "report_modal"},
            {"components", json::array({
                json{{"type", 18}, {"component", {
                    {"type", 4}, {"custom_id", "subject"}, {"value", "Bug"}}}},
                json{{"type", 18}, {"component", {
                    {"type", 4}, {"custom_id", "details"}, {"value", "It broke"}}}},
            })},
        }},
    };
    CHECK(modal_value(interaction, "subject").value() == "Bug");
    CHECK(modal_value(interaction, "details").value() == "It broke");
    CHECK_FALSE(modal_value(interaction, "nope").has_value());
}

// ---------------------------------------------------------------------------
// custom_id clamp integrity + text_input length-range validation
// ---------------------------------------------------------------------------

TEST_CASE("text_input clamps a 101-byte custom_id to exactly 100 bytes with no ellipsis") {
    const std::string long_id(101, 'x');
    const json input = text_input(long_id, TextInputStyle::short_input);
    const std::string clamped = input["custom_id"].get<std::string>();
    CHECK(clamped.size() == 100u);
    CHECK(clamped == std::string(100, 'x'));
    CHECK(clamped.find("\xE2\x80\xA6") == std::string::npos);
}

TEST_CASE("modal clamps its own custom_id without an ellipsis") {
    const std::string long_id(101, 'm');
    const json response = modal(long_id, "Title", {
        label_component("Q", text_input("q", TextInputStyle::short_input)),
    });
    const std::string clamped = response["data"]["custom_id"].get<std::string>();
    CHECK(clamped.size() == 100u);
    CHECK(clamped == std::string(100, 'm'));
    CHECK(clamped.find("\xE2\x80\xA6") == std::string::npos);
}

TEST_CASE("text_input rejects out-of-range min_length") {
    CHECK_THROWS_AS(text_input("f", TextInputStyle::short_input, true, "", "", -1, 0),
                    ModuleError);
    CHECK_THROWS_AS(text_input("f", TextInputStyle::short_input, true, "", "", 4001, 0),
                    ModuleError);
}

TEST_CASE("text_input rejects out-of-range max_length") {
    // max_length == 0 means "omit"; a set max_length must be 1..4000.
    CHECK_THROWS_AS(text_input("f", TextInputStyle::short_input, true, "", "", 0, -1),
                    ModuleError);
    CHECK_THROWS_AS(text_input("f", TextInputStyle::short_input, true, "", "", 0, 4001),
                    ModuleError);
}

TEST_CASE("text_input rejects min_length greater than max_length when both set") {
    try {
        text_input("f", TextInputStyle::short_input, true, "", "", 5, 3);
        FAIL("expected ModuleError");
    } catch (const ModuleError& e) {
        CHECK(e.category == discord_interactions::ErrorCategory::validation);
    }
}

TEST_CASE("text_input accepts boundary length ranges") {
    CHECK_NOTHROW(text_input("f", TextInputStyle::short_input, true, "", "", 0, 0));
    CHECK_NOTHROW(text_input("f", TextInputStyle::short_input, true, "", "", 0, 4000));
    CHECK_NOTHROW(text_input("f", TextInputStyle::short_input, true, "", "", 4000, 4000));
    CHECK_NOTHROW(text_input("f", TextInputStyle::short_input, true, "", "", 1, 1));
}
