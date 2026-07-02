#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

#include "discord_interactions/components.hpp"
#include "discord_interactions/components_v2.hpp"
#include "discord_interactions/errors.hpp"
#include "discord_interactions/limits.hpp"

using nlohmann::json;
using discord_interactions::text_display;
using discord_interactions::thumbnail_component;
using discord_interactions::section;
using discord_interactions::media_gallery;
using discord_interactions::file_component;
using discord_interactions::separator;
using discord_interactions::container;
using discord_interactions::components_v2_message;
using discord_interactions::message_flag_components_v2;
using discord_interactions::button;
using discord_interactions::ButtonStyle;
using discord_interactions::action_row;
using discord_interactions::ModuleError;
using discord_interactions::ErrorCategory;
namespace limits = discord_interactions::limits;

TEST_CASE("message_flag_components_v2 equals 1 << 15") {
    CHECK(message_flag_components_v2 == (1 << 15));
    CHECK(message_flag_components_v2 == 32768);
}

TEST_CASE("text_display emits type 10 with content") {
    const json t = text_display("Hello world");
    CHECK(t["type"] == 10);
    CHECK(t["content"] == "Hello world");
}

TEST_CASE("text_display clamps content to text_display_content (4000)") {
    const std::string big(5000, 'a');
    const json t = text_display(big);
    const std::string content = t["content"].get<std::string>();
    CHECK(content.size() <= limits::text_display_content);
    CHECK(content.size() < big.size());
}

TEST_CASE("thumbnail_component emits type 11 with media url, defaults omitted") {
    const json th = thumbnail_component("https://cdn/img.png");
    CHECK(th["type"] == 11);
    CHECK(th["media"]["url"] == "https://cdn/img.png");
    CHECK_FALSE(th.contains("description"));
    CHECK_FALSE(th.contains("spoiler"));
}

TEST_CASE("thumbnail_component emits description and spoiler when set") {
    const json th = thumbnail_component("https://cdn/img.png", "alt text", true);
    CHECK(th["type"] == 11);
    CHECK(th["media"]["url"] == "https://cdn/img.png");
    CHECK(th["description"] == "alt text");
    CHECK(th["spoiler"] == true);
}

TEST_CASE("section emits type 9 with one text display and button accessory") {
    const json texts = json::array({text_display("Line one")});
    const json accessory = button(ButtonStyle::primary, "act", "Act");
    const json s = section(texts, accessory);
    CHECK(s["type"] == 9);
    CHECK(s["components"] == texts);
    CHECK(s["components"].size() == 1);
    CHECK(s["accessory"] == accessory);
}

TEST_CASE("section accepts three text displays") {
    const json texts = json::array({text_display("a"), text_display("b"), text_display("c")});
    const json accessory = thumbnail_component("https://cdn/x.png");
    const json s = section(texts, accessory);
    CHECK(s["type"] == 9);
    CHECK(s["components"].size() == 3);
    CHECK(s["accessory"] == accessory);
}

TEST_CASE("section throws on zero text displays") {
    const json accessory = button(ButtonStyle::primary, "a", "A");
    try {
        section(json::array(), accessory);
        FAIL("expected ModuleError");
    } catch (const ModuleError& e) {
        CHECK(e.category == ErrorCategory::validation);
    }
}

TEST_CASE("section throws on four text displays") {
    const json texts = json::array({text_display("a"), text_display("b"),
                                    text_display("c"), text_display("d")});
    const json accessory = button(ButtonStyle::primary, "a", "A");
    try {
        section(texts, accessory);
        FAIL("expected ModuleError");
    } catch (const ModuleError& e) {
        CHECK(e.category == ErrorCategory::validation);
    }
}

TEST_CASE("media_gallery emits type 12 with items") {
    const json items = json::array({json{{"media", {{"url", "https://cdn/1.png"}}}}});
    const json g = media_gallery(items);
    CHECK(g["type"] == 12);
    CHECK(g["items"] == items);
}

TEST_CASE("media_gallery accepts 1 and 10 items") {
    json one = json::array({json{{"media", {{"url", "https://cdn/1.png"}}}}});
    CHECK(media_gallery(one)["items"].size() == 1);

    json ten = json::array();
    for (int i = 0; i < 10; ++i) {
        ten.push_back(json{{"media", {{"url", "https://cdn/" + std::to_string(i) + ".png"}}}});
    }
    CHECK(media_gallery(ten)["items"].size() == 10);
}

TEST_CASE("media_gallery throws on 0 and 11 items") {
    try {
        media_gallery(json::array());
        FAIL("expected ModuleError");
    } catch (const ModuleError& e) {
        CHECK(e.category == ErrorCategory::validation);
    }

    json eleven = json::array();
    for (int i = 0; i < 11; ++i) {
        eleven.push_back(json{{"media", {{"url", "https://cdn/" + std::to_string(i) + ".png"}}}});
    }
    try {
        media_gallery(eleven);
        FAIL("expected ModuleError");
    } catch (const ModuleError& e) {
        CHECK(e.category == ErrorCategory::validation);
    }
}

TEST_CASE("file_component emits type 13 with file url, spoiler omitted by default") {
    const json f = file_component("attachment://report.pdf");
    CHECK(f["type"] == 13);
    CHECK(f["file"]["url"] == "attachment://report.pdf");
    CHECK_FALSE(f.contains("spoiler"));
}

TEST_CASE("file_component emits spoiler when set") {
    const json f = file_component("attachment://report.pdf", true);
    CHECK(f["type"] == 13);
    CHECK(f["spoiler"] == true);
}

TEST_CASE("separator emits type 14 with divider and spacing defaults") {
    const json s = separator();
    CHECK(s["type"] == 14);
    CHECK(s["divider"] == true);
    CHECK(s["spacing"] == 1);
}

TEST_CASE("separator spacing 2 and divider false") {
    const json s = separator(false, 2);
    CHECK(s["type"] == 14);
    CHECK(s["divider"] == false);
    CHECK(s["spacing"] == 2);
}

TEST_CASE("separator throws on invalid spacing") {
    try {
        separator(true, 0);
        FAIL("expected ModuleError");
    } catch (const ModuleError& e) {
        CHECK(e.category == ErrorCategory::validation);
    }
    try {
        separator(true, 3);
        FAIL("expected ModuleError");
    } catch (const ModuleError& e) {
        CHECK(e.category == ErrorCategory::validation);
    }
}

TEST_CASE("container emits type 17 with accent color present") {
    const json comps = json::array({text_display("hi")});
    const json c = container(comps, 0x5865F2u);
    CHECK(c["type"] == 17);
    CHECK(c["components"] == comps);
    CHECK(c["accent_color"] == 0x5865F2u);
    CHECK_FALSE(c.contains("spoiler"));
}

TEST_CASE("container omits accent color when nullopt, emits spoiler") {
    const json comps = json::array({text_display("hi")});
    const json c = container(comps, std::nullopt, true);
    CHECK(c["type"] == 17);
    CHECK_FALSE(c.contains("accent_color"));
    CHECK(c["spoiler"] == true);
}

TEST_CASE("components_v2_message assembles flag and components") {
    const json comps = json::array({text_display("hello")});
    const json m = components_v2_message(comps);
    CHECK(m["flags"] == message_flag_components_v2);
    CHECK(m["components"] == comps);
    CHECK_FALSE(m.contains("content"));
    CHECK_FALSE(m.contains("embeds"));
}

TEST_CASE("components_v2_message allows exactly 40 recursive components") {
    // container(1) + 39 text displays = 40 total.
    json children = json::array();
    for (int i = 0; i < 39; ++i) {
        children.push_back(text_display("t" + std::to_string(i)));
    }
    const json comps = json::array({container(children)});
    const json m = components_v2_message(comps);
    CHECK(m["flags"] == message_flag_components_v2);
    CHECK(m["components"].size() == 1);
}

TEST_CASE("components_v2_message throws too_many_components_v2 past 40") {
    // container(1) + 40 text displays = 41 total.
    json children = json::array();
    for (int i = 0; i < 40; ++i) {
        children.push_back(text_display("t" + std::to_string(i)));
    }
    const json comps = json::array({container(children)});
    try {
        components_v2_message(comps);
        FAIL("expected ModuleError");
    } catch (const ModuleError& e) {
        CHECK(e.code == "too_many_components_v2");
        CHECK(e.category == ErrorCategory::validation);
    }
}

TEST_CASE("action_row from components.hpp composes inside a container") {
    const json row = action_row(json::array({button(ButtonStyle::primary, "go", "Go")}));
    const json c = container(json::array({row}));
    const json m = components_v2_message(json::array({c}));
    CHECK(m["flags"] == message_flag_components_v2);
    CHECK(m["components"][0]["type"] == 17);
    CHECK(m["components"][0]["components"][0]["type"] == 1);
    CHECK(m["components"][0]["components"][0]["components"][0]["type"] == 2);
}
