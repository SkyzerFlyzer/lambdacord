#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "discord_interactions/modal.hpp"

#include <optional>
#include <string>
#include <vector>

using nlohmann::json;
using discord_interactions::TextInputStyle;
using discord_interactions::text_input;
using discord_interactions::label_component;
using discord_interactions::modal;
using discord_interactions::file_upload;
using discord_interactions::radio_option;
using discord_interactions::radio_group;
using discord_interactions::checkbox_group;
using discord_interactions::checkbox;
using discord_interactions::modal_values;
using discord_interactions::modal_checked;

// ---------------------------------------------------------------------------
// Factory: File Upload (type 19)
// ---------------------------------------------------------------------------

TEST_CASE("file_upload emits type 19 with custom_id and required default true") {
    const json input = file_upload("resume");
    CHECK(input["type"] == 19);
    CHECK(input["custom_id"] == "resume");
    CHECK(input["required"] == true);
    // The bare input never carries its own label (that lives on the Label wrapper).
    CHECK_FALSE(input.contains("label"));
}

TEST_CASE("file_upload honors required=false") {
    const json input = file_upload("attachment", false);
    CHECK(input["type"] == 19);
    CHECK(input["required"] == false);
}

TEST_CASE("file_upload clamps custom_id to limits::custom_id") {
    const std::string long_id(150, 'f');
    const json input = file_upload(long_id);
    CHECK(input["custom_id"].get<std::string>().size() <= 100u);
}

// ---------------------------------------------------------------------------
// Factory: Radio option + Radio Group (type 21)
// ---------------------------------------------------------------------------

TEST_CASE("radio_option emits label and value and omits optional keys when defaulted") {
    const json option = radio_option("Small", "sm");
    CHECK(option["label"] == "Small");
    CHECK(option["value"] == "sm");
    CHECK_FALSE(option.contains("description"));
    CHECK_FALSE(option.contains("default"));
}

TEST_CASE("radio_option emits description and default when provided") {
    const json option = radio_option("Large", "lg", "The big one", true);
    CHECK(option["description"] == "The big one");
    CHECK(option["default"] == true);
}

TEST_CASE("radio_group emits type 21 with options and required, plus a default option") {
    const json options = json::array({
        radio_option("Yes", "y"),
        radio_option("No", "n", "", true),
    });
    const json group = radio_group("consent", options);
    CHECK(group["type"] == 21);
    CHECK(group["custom_id"] == "consent");
    CHECK(group["required"] == true);
    REQUIRE(group["options"].is_array());
    CHECK(group["options"].size() == 2);
    // The default flag survives into the group's option array.
    CHECK(group["options"][1]["default"] == true);
}

TEST_CASE("radio_group honors required=false and clamps custom_id") {
    const std::string long_id(150, 'r');
    const json group = radio_group(
        long_id, json::array({radio_option("a", "a"), radio_option("b", "b")}), false);
    CHECK(group["required"] == false);
    CHECK(group["custom_id"].get<std::string>().size() <= 100u);
}

TEST_CASE("radio_group rejects fewer than 2 options") {
    using discord_interactions::ModuleError;
    CHECK_THROWS_AS(radio_group("r", json::array({radio_option("a", "a")})),
                    ModuleError);
    // A non-array options value is also rejected.
    CHECK_THROWS_AS(radio_group("r", json::object()), ModuleError);
}

TEST_CASE("radio_group rejects more than 10 options") {
    using discord_interactions::ModuleError;
    json options = json::array();
    for (int i = 0; i < 11; ++i) {
        options.push_back(radio_option("L" + std::to_string(i), std::to_string(i)));
    }
    CHECK_THROWS_AS(radio_group("r", options), ModuleError);
}

TEST_CASE("radio_group accepts exactly 2 and exactly 10 options") {
    using discord_interactions::radio_group;
    CHECK_NOTHROW(
        radio_group("r", json::array({radio_option("a", "a"), radio_option("b", "b")})));
    json ten = json::array();
    for (int i = 0; i < 10; ++i) {
        ten.push_back(radio_option("L" + std::to_string(i), std::to_string(i)));
    }
    CHECK_NOTHROW(radio_group("r", ten));
}

// ---------------------------------------------------------------------------
// Factory: Checkbox Group (type 22)
// ---------------------------------------------------------------------------

TEST_CASE("checkbox_group emits type 22 with options and required, min/max omitted") {
    const json options = json::array({
        radio_option("Email", "email"),
        radio_option("SMS", "sms"),
    });
    const json group = checkbox_group("notify", options);
    CHECK(group["type"] == 22);
    CHECK(group["custom_id"] == "notify");
    CHECK(group["required"] == true);
    CHECK(group["options"].size() == 2);
    // min_values/max_values are only emitted when explicitly set (additive fields).
    CHECK_FALSE(group.contains("min_values"));
    CHECK_FALSE(group.contains("max_values"));
}

TEST_CASE("checkbox_group emits min_values/max_values when supplied") {
    const json group = checkbox_group("pick", json::array({radio_option("a", "a")}),
                                      true, 1, 3);
    CHECK(group["min_values"] == 1);
    CHECK(group["max_values"] == 3);
}

TEST_CASE("checkbox_group rejects min_values > max_values") {
    using discord_interactions::ModuleError;
    CHECK_THROWS_AS(
        checkbox_group("pick", json::array({radio_option("a", "a")}), true, 3, 1),
        ModuleError);
}

TEST_CASE("checkbox_group rejects a negative min or max other than the -1 sentinel") {
    using discord_interactions::ModuleError;
    // -1 means "omit"; -2 is an out-of-range value.
    CHECK_THROWS_AS(
        checkbox_group("pick", json::array({radio_option("a", "a")}), true, -2, 3),
        ModuleError);
}

TEST_CASE("checkbox_group rejects max_values above limits::checkbox_group_max_values") {
    using discord_interactions::ModuleError;
    CHECK_THROWS_AS(
        checkbox_group("pick", json::array({radio_option("a", "a")}), true, 0, 11),
        ModuleError);
}

TEST_CASE("checkbox_group accepts a boundary 0..10 range when not required") {
    // min_values: 0 is only legal on a non-required group; a required group
    // must have min_values omitted or >= 1 (Discord's documented constraint).
    CHECK_NOTHROW(
        checkbox_group("pick", json::array({radio_option("a", "a")}), false, 0, 10));
    CHECK_NOTHROW(
        checkbox_group("pick", json::array({radio_option("a", "a")}), true, 1, 10));
}

TEST_CASE("checkbox_group rejects min_values 0 when required") {
    using discord_interactions::ModuleError;
    CHECK_THROWS_AS(
        checkbox_group("pick", json::array({radio_option("a", "a")}), true, 0, 10),
        ModuleError);
    // required with min_values omitted stays legal.
    CHECK_NOTHROW(
        checkbox_group("pick", json::array({radio_option("a", "a")}), true, -1, 10));
}

TEST_CASE("checkbox_group rejects a set max_values below 1") {
    using discord_interactions::ModuleError;
    CHECK_THROWS_AS(
        checkbox_group("pick", json::array({radio_option("a", "a")}), false, -1, 0),
        ModuleError);
}

TEST_CASE("checkbox_group rejects non-array, empty, and oversized option lists") {
    using discord_interactions::ModuleError;
    CHECK_THROWS_AS(checkbox_group("pick", json::object()), ModuleError);
    CHECK_THROWS_AS(checkbox_group("pick", json("nope")), ModuleError);
    CHECK_THROWS_AS(checkbox_group("pick", json::array()), ModuleError);
    json eleven = json::array();
    for (int i = 0; i < 11; ++i) {
        eleven.push_back(radio_option("L" + std::to_string(i), std::to_string(i)));
    }
    CHECK_THROWS_AS(checkbox_group("pick", eleven), ModuleError);
}

TEST_CASE("checkbox_group accepts 1 and 10 options") {
    CHECK_NOTHROW(checkbox_group("pick", json::array({radio_option("a", "a")})));
    json ten = json::array();
    for (int i = 0; i < 10; ++i) {
        ten.push_back(radio_option("L" + std::to_string(i), std::to_string(i)));
    }
    CHECK_NOTHROW(checkbox_group("pick", ten));
}

TEST_CASE("modal input custom_ids clamp to 100 bytes without an ellipsis") {
    const std::string long_id(101, 'k');
    const std::string expected(100, 'k');
    const json opts = json::array({radio_option("a", "a"), radio_option("b", "b")});
    for (const json& input : {file_upload(long_id), radio_group(long_id, opts),
                              checkbox_group(long_id, opts), checkbox(long_id)}) {
        const std::string clamped = input["custom_id"].get<std::string>();
        CHECK(clamped.size() == 100u);
        CHECK(clamped == expected);
        CHECK(clamped.find("\xE2\x80\xA6") == std::string::npos);
    }
}

// ---------------------------------------------------------------------------
// Factory: Checkbox (type 23)
// ---------------------------------------------------------------------------

TEST_CASE("checkbox emits type 23 and NEVER emits a required field") {
    const json box = checkbox("agree");
    CHECK(box["type"] == 23);
    CHECK(box["custom_id"] == "agree");
    // Standalone Checkbox: the docs are ambiguous about a "required" field, so
    // it is never emitted (Discord's default applies if the field exists).
    CHECK_FALSE(box.contains("required"));
    CHECK_FALSE(box.contains("default"));
    // The checkbox's visible text is supplied by its Label wrapper.
    CHECK_FALSE(box.contains("label"));
}

TEST_CASE("checkbox never emits required even when the required param is set") {
    // The required parameter is retained for source compatibility but ignored.
    const json box = checkbox("terms", true, true);
    CHECK_FALSE(box.contains("required"));
    CHECK(box["default"] == true);
}

// ---------------------------------------------------------------------------
// Extraction: modal_values (checkbox group / file upload / absent)
// ---------------------------------------------------------------------------

TEST_CASE("modal_values extracts checkbox-group selections through a Label wrapper") {
    const json interaction = json{
        {"type", 5},
        {"data", {
            {"custom_id", "prefs_modal"},
            {"components", json::array({
                json{{"type", 18}, {"component", {
                    {"type", 22}, {"custom_id", "notify"},
                    {"values", json::array({"email", "sms"})}}}},
            })},
        }},
    };
    const std::vector<std::string> values = modal_values(interaction, "notify");
    REQUIRE(values.size() == 2);
    CHECK(values[0] == "email");
    CHECK(values[1] == "sms");
}

TEST_CASE("modal_values extracts file-upload values") {
    // Discord delivers a submitted File Upload's attachment snowflakes under
    // "values" — the same key a Checkbox Group uses.
    const json interaction = json{
        {"type", 5},
        {"data", {
            {"custom_id", "upload_modal"},
            {"components", json::array({
                json{{"type", 18}, {"component", {
                    {"type", 19}, {"custom_id", "resume"},
                    {"values", json::array({"1234567890", "9876543210"})}}}},
            })},
        }},
    };
    const std::vector<std::string> ids = modal_values(interaction, "resume");
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] == "1234567890");
    CHECK(ids[1] == "9876543210");
}

TEST_CASE("modal_values ignores the undocumented attachment_ids key") {
    // "attachment_ids" does not exist in Discord's documented MODAL_SUBMIT
    // shape (file uploads submit "values"); the extractor must not read it.
    const json interaction = json{
        {"type", 5},
        {"data", {
            {"custom_id", "upload_modal"},
            {"components", json::array({
                json{{"type", 18}, {"component", {
                    {"type", 19}, {"custom_id", "resume"},
                    {"attachment_ids", json::array({"1234567890"})}}}},
            })},
        }},
    };
    CHECK(modal_values(interaction, "resume").empty());
}

TEST_CASE("modal_values returns an empty vector for an absent custom_id") {
    const json interaction = json{
        {"type", 5},
        {"data", {
            {"custom_id", "prefs_modal"},
            {"components", json::array({
                json{{"type", 18}, {"component", {
                    {"type", 22}, {"custom_id", "notify"},
                    {"values", json::array({"email"})}}}},
            })},
        }},
    };
    CHECK(modal_values(interaction, "missing").empty());
    // Missing data entirely is handled too.
    CHECK(modal_values(json{{"type", 5}}, "notify").empty());
}

// ---------------------------------------------------------------------------
// Extraction: modal_checked (true / false / absent)
// ---------------------------------------------------------------------------

TEST_CASE("modal_checked reports a checked checkbox as true through a Label wrapper") {
    const json interaction = json{
        {"type", 5},
        {"data", {
            {"custom_id", "consent_modal"},
            {"components", json::array({
                json{{"type", 18}, {"component", {
                    {"type", 23}, {"custom_id", "agree"}, {"value", true}}}},
            })},
        }},
    };
    const std::optional<bool> checked = modal_checked(interaction, "agree");
    REQUIRE(checked.has_value());
    CHECK(checked.value() == true);
}

TEST_CASE("modal_checked reports an unchecked checkbox as false") {
    const json interaction = json{
        {"type", 5},
        {"data", {
            {"custom_id", "consent_modal"},
            {"components", json::array({
                json{{"type", 18}, {"component", {
                    {"type", 23}, {"custom_id", "agree"}, {"value", false}}}},
            })},
        }},
    };
    const std::optional<bool> checked = modal_checked(interaction, "agree");
    REQUIRE(checked.has_value());
    CHECK(checked.value() == false);
}

TEST_CASE("modal_checked returns nullopt when the checkbox is absent") {
    const json interaction = json{
        {"type", 5},
        {"data", {
            {"custom_id", "consent_modal"},
            {"components", json::array({
                json{{"type", 18}, {"component", {
                    {"type", 23}, {"custom_id", "agree"}, {"value", true}}}},
            })},
        }},
    };
    CHECK_FALSE(modal_checked(interaction, "missing").has_value());
    CHECK_FALSE(modal_checked(json{{"type", 5}}, "agree").has_value());
}

// ---------------------------------------------------------------------------
// Real-shaped multi-input MODAL_SUBMIT extraction
// ---------------------------------------------------------------------------

TEST_CASE("extraction works over a real-shaped multi-input MODAL_SUBMIT fixture") {
    const json interaction = json{
        {"type", 5},
        {"data", {
            {"custom_id", "onboarding_modal"},
            {"components", json::array({
                json{{"type", 18}, {"label", "Name"}, {"component", {
                    {"type", 4}, {"custom_id", "name"}, {"value", "Ada"}}}},
                json{{"type", 18}, {"label", "Avatar"}, {"component", {
                    {"type", 19}, {"custom_id", "avatar"},
                    {"values", json::array({"555"})}}}},
                json{{"type", 18}, {"label", "Channels"}, {"component", {
                    {"type", 22}, {"custom_id", "channels"},
                    {"values", json::array({"general", "random"})}}}},
                json{{"type", 18}, {"label", "Accept ToS"}, {"component", {
                    {"type", 23}, {"custom_id", "tos"}, {"value", true}}}},
            })},
        }},
    };
    CHECK(modal_values(interaction, "avatar") == std::vector<std::string>{"555"});
    CHECK(modal_values(interaction, "channels") ==
          std::vector<std::string>{"general", "random"});
    REQUIRE(modal_checked(interaction, "tos").has_value());
    CHECK(modal_checked(interaction, "tos").value() == true);
}

// ---------------------------------------------------------------------------
// Composition: a modal with one of each input type
// ---------------------------------------------------------------------------

TEST_CASE("modal composes one of each input type wrapped in Labels and fits in 5") {
    const json response = modal("everything", "All Inputs", {
        label_component("Feedback", text_input("fb", TextInputStyle::paragraph)),
        label_component("Resume", file_upload("resume")),
        label_component("Size", radio_group("size", json::array({
            radio_option("Small", "sm"), radio_option("Large", "lg", "", true)}))),
        label_component("Notify", checkbox_group("notify", json::array({
            radio_option("Email", "email"), radio_option("SMS", "sms")}))),
        label_component("Accept", checkbox("agree")),
    });
    CHECK(response["type"] == 9);
    REQUIRE(response["data"]["components"].is_array());
    CHECK(response["data"]["components"].size() == 5);
    CHECK(response["data"]["components"].size() <= 5u);
    // Every child is Label-wrapped (type 18) and no deprecated action row is present.
    for (const auto& child : response["data"]["components"]) {
        CHECK(child["type"] == 18);
    }
}
