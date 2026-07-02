#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "discord_interactions/interaction.hpp"

using nlohmann::json;
using discord_interactions::ApplicationCommandKind;
using discord_interactions::application_command_kind;
using discord_interactions::context_menu_route_suffix;

TEST_CASE("application_command_kind detects chat input for data.type 1") {
    const json interaction = json{{"data", {{"type", 1}, {"name", "ping"}}}};
    CHECK(application_command_kind(interaction) == ApplicationCommandKind::chat_input);
}

TEST_CASE("application_command_kind detects user commands for data.type 2") {
    const json interaction = json{{"data", {{"type", 2}, {"name", "Report User"}}}};
    CHECK(application_command_kind(interaction) == ApplicationCommandKind::user);
}

TEST_CASE("application_command_kind detects message commands for data.type 3") {
    const json interaction = json{{"data", {{"type", 3}, {"name", "Pin Message"}}}};
    CHECK(application_command_kind(interaction) == ApplicationCommandKind::message);
}

TEST_CASE("application_command_kind defaults to chat input when data.type is absent") {
    const json interaction = json{{"data", {{"name", "ping"}}}};
    CHECK(application_command_kind(interaction) == ApplicationCommandKind::chat_input);
}

TEST_CASE("application_command_kind defaults to chat input for garbage data.type") {
    SUBCASE("string type") {
        const json interaction = json{{"data", {{"type", "user"}, {"name", "x"}}}};
        CHECK(application_command_kind(interaction) == ApplicationCommandKind::chat_input);
    }
    SUBCASE("unknown numeric type") {
        const json interaction = json{{"data", {{"type", 42}, {"name", "x"}}}};
        CHECK(application_command_kind(interaction) == ApplicationCommandKind::chat_input);
    }
    SUBCASE("null type") {
        const json interaction = json{{"data", {{"type", nullptr}, {"name", "x"}}}};
        CHECK(application_command_kind(interaction) == ApplicationCommandKind::chat_input);
    }
    SUBCASE("data missing entirely") {
        const json interaction = json::object();
        CHECK(application_command_kind(interaction) == ApplicationCommandKind::chat_input);
    }
    SUBCASE("data is not an object") {
        const json interaction = json{{"data", "nope"}};
        CHECK(application_command_kind(interaction) == ApplicationCommandKind::chat_input);
    }
}

TEST_CASE("context_menu_route_suffix lowercases and replaces spaces") {
    CHECK(context_menu_route_suffix("Report User") == "report-user");
}

TEST_CASE("context_menu_route_suffix maps every space to a dash") {
    // Each space becomes exactly one '-'; runs of spaces are not collapsed.
    CHECK(context_menu_route_suffix("Report  User") == "report--user");
    CHECK(context_menu_route_suffix("A B C") == "a-b-c");
}

TEST_CASE("context_menu_route_suffix leaves already-normalized names unchanged") {
    CHECK(context_menu_route_suffix("report-user") == "report-user");
    CHECK(context_menu_route_suffix("") == "");
}

TEST_CASE("context_menu_route_suffix lowercases ASCII only, preserving other bytes") {
    // Lowercasing is byte-wise ASCII only: multi-byte UTF-8 sequences pass
    // through untouched (no Unicode case folding), so 'Ü' stays 'Ü' while the
    // ASCII letters around it still lowercase and spaces still become dashes.
    CHECK(context_menu_route_suffix("Süß Report") == "süß-report");
    CHECK(context_menu_route_suffix("Übersetzen") == "Übersetzen");
    CHECK(context_menu_route_suffix("メッセージ通報") == "メッセージ通報");
    CHECK(context_menu_route_suffix("Report メッセージ") == "report-メッセージ");
}
