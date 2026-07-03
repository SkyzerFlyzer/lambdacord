#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "discord_interactions/options.hpp"

using nlohmann::json;
namespace di = discord_interactions;

namespace {

// Top-level CHAT_INPUT command with one option of every simple type plus
// snowflake-valued options and a resolved block. Real-shaped Discord payload.
json top_level_interaction() {
    return json{
        {"type", 2},
        {"data",
         {{"id", "cmd1"},
          {"name", "config"},
          {"type", 1},
          {"options",
           json::array(
               {json{{"name", "text"}, {"type", 3}, {"value", "hello"}},
                json{{"name", "count"}, {"type", 4}, {"value", 42}},
                json{{"name", "ratio"}, {"type", 10}, {"value", 3.5}},
                json{{"name", "enabled"}, {"type", 5}, {"value", true}},
                json{{"name", "who"}, {"type", 6}, {"value", "111222333444555666"}},
                json{{"name", "where"}, {"type", 7}, {"value", "777888999000111222"}},
                json{{"name", "role"}, {"type", 8}, {"value", "333444555666777888"}},
                json{{"name", "file"}, {"type", 11}, {"value", "999000111222333444"}}})},
          {"resolved",
           {{"users",
             {{"111222333444555666",
               {{"id", "111222333444555666"}, {"username", "alice"}}}}},
            {"members",
             {{"111222333444555666",
               {{"nick", "Al"}, {"roles", json::array()}}}}},
            {"channels",
             {{"777888999000111222",
               {{"id", "777888999000111222"}, {"name", "general"}, {"type", 0}}}}},
            {"roles",
             {{"333444555666777888",
               {{"id", "333444555666777888"}, {"name", "admins"}}}}},
            {"attachments",
             {{"999000111222333444",
               {{"id", "999000111222333444"}, {"filename", "a.png"}}}}}}}}}};
}

// One-level subcommand: /account link  (SUB_COMMAND type 1 wrapping options).
json subcommand_interaction() {
    return json{
        {"type", 2},
        {"data",
         {{"name", "account"},
          {"type", 1},
          {"options",
           json::array({json{
               {"name", "link"},
               {"type", 1},
               {"options",
                json::array({json{{"name", "handle"}, {"type", 3}, {"value", "bob"}}})}}})}}}};
}

// Group + subcommand nesting: SUB_COMMAND_GROUP (2) -> SUB_COMMAND (1) -> options.
json group_subcommand_interaction() {
    return json{
        {"type", 2},
        {"data",
         {{"name", "settings"},
          {"type", 1},
          {"options",
           json::array({json{
               {"name", "notify"},
               {"type", 2},
               {"options",
                json::array({json{
                    {"name", "add"},
                    {"type", 1},
                    {"options",
                     json::array(
                         {json{{"name", "minutes"}, {"type", 4}, {"value", 15}}})}}})}}})}}}};
}

}  // namespace

TEST_CASE("command_options returns top-level option list") {
    const json list = di::command_options(top_level_interaction());
    REQUIRE(list.is_array());
    CHECK(list.size() == 8);
    CHECK(list.at(0).at("name") == "text");
}

TEST_CASE("command_options walks a one-level subcommand") {
    const json list = di::command_options(subcommand_interaction());
    REQUIRE(list.is_array());
    CHECK(list.size() == 1);
    CHECK(list.at(0).at("name") == "handle");
}

TEST_CASE("command_options walks group + subcommand nesting") {
    const json list = di::command_options(group_subcommand_interaction());
    REQUIRE(list.is_array());
    CHECK(list.size() == 1);
    CHECK(list.at(0).at("name") == "minutes");
}

TEST_CASE("command_options with no data returns empty array") {
    const json list = di::command_options(json{{"type", 2}});
    CHECK(list.is_array());
    CHECK(list.empty());
}

TEST_CASE("option_string happy path") {
    CHECK(di::option_string(top_level_interaction(), "text") == std::string("hello"));
}

TEST_CASE("option_int happy path") {
    CHECK(di::option_int(top_level_interaction(), "count") == int64_t(42));
}

TEST_CASE("option_number happy path") {
    const auto v = di::option_number(top_level_interaction(), "ratio");
    REQUIRE(v.has_value());
    CHECK(v.value() == doctest::Approx(3.5));
}

TEST_CASE("option_bool happy path") {
    CHECK(di::option_bool(top_level_interaction(), "enabled") == true);
}

TEST_CASE("option_id resolves snowflake option types 6/7/8/11") {
    CHECK(di::option_id(top_level_interaction(), "who") ==
          std::string("111222333444555666"));
    CHECK(di::option_id(top_level_interaction(), "where") ==
          std::string("777888999000111222"));
    CHECK(di::option_id(top_level_interaction(), "role") ==
          std::string("333444555666777888"));
    CHECK(di::option_id(top_level_interaction(), "file") ==
          std::string("999000111222333444"));
}

TEST_CASE("option getters work through nested subcommand options") {
    CHECK(di::option_string(subcommand_interaction(), "handle") == std::string("bob"));
    CHECK(di::option_int(group_subcommand_interaction(), "minutes") == int64_t(15));
}

TEST_CASE("type mismatch returns nullopt and never throws") {
    // count is INTEGER; asking for string must be nullopt, not a throw.
    CHECK_FALSE(di::option_string(top_level_interaction(), "count").has_value());
    // text is STRING; asking for int must be nullopt.
    CHECK_FALSE(di::option_int(top_level_interaction(), "text").has_value());
    // enabled is BOOLEAN; asking for number must be nullopt.
    CHECK_FALSE(di::option_number(top_level_interaction(), "enabled").has_value());
    // text is STRING; asking for bool must be nullopt.
    CHECK_FALSE(di::option_bool(top_level_interaction(), "text").has_value());
    // text is STRING (not a snowflake option type); option_id nullopt.
    CHECK_FALSE(di::option_id(top_level_interaction(), "text").has_value());
}

TEST_CASE("absent option returns nullopt for every getter") {
    const json it = top_level_interaction();
    CHECK_FALSE(di::option_string(it, "missing").has_value());
    CHECK_FALSE(di::option_int(it, "missing").has_value());
    CHECK_FALSE(di::option_number(it, "missing").has_value());
    CHECK_FALSE(di::option_bool(it, "missing").has_value());
    CHECK_FALSE(di::option_id(it, "missing").has_value());
}

TEST_CASE("getters on interaction with no data return nullopt") {
    const json it = json{{"type", 2}};
    CHECK_FALSE(di::option_string(it, "text").has_value());
    CHECK_FALSE(di::option_int(it, "count").has_value());
    CHECK_FALSE(di::option_number(it, "ratio").has_value());
    CHECK_FALSE(di::option_bool(it, "enabled").has_value());
    CHECK_FALSE(di::option_id(it, "who").has_value());
}

TEST_CASE("option_int on a non-integral NUMBER value is a mismatch (nullopt)") {
    // ratio = 3.5 is not integral; reading as int must be nullopt, not truncate.
    CHECK_FALSE(di::option_int(top_level_interaction(), "ratio").has_value());
}

TEST_CASE("resolved_user lookup") {
    const json u = di::resolved_user(top_level_interaction(), "111222333444555666");
    CHECK(u.value("username", "") == "alice");
    CHECK(di::resolved_user(top_level_interaction(), "nope").empty());
}

TEST_CASE("resolved_member lookup") {
    const json m = di::resolved_member(top_level_interaction(), "111222333444555666");
    CHECK(m.value("nick", "") == "Al");
    CHECK(di::resolved_member(top_level_interaction(), "nope").empty());
}

TEST_CASE("resolved_channel lookup") {
    const json c = di::resolved_channel(top_level_interaction(), "777888999000111222");
    CHECK(c.value("name", "") == "general");
    CHECK(di::resolved_channel(top_level_interaction(), "nope").empty());
}

TEST_CASE("resolved_role lookup") {
    const json r = di::resolved_role(top_level_interaction(), "333444555666777888");
    CHECK(r.value("name", "") == "admins");
    CHECK(di::resolved_role(top_level_interaction(), "nope").empty());
}

TEST_CASE("resolved_attachment lookup") {
    const json a = di::resolved_attachment(top_level_interaction(), "999000111222333444");
    CHECK(a.value("filename", "") == "a.png");
    CHECK(di::resolved_attachment(top_level_interaction(), "nope").empty());
}

TEST_CASE("resolved lookups on interaction with no data return empty") {
    const json it = json{{"type", 2}};
    CHECK(di::resolved_user(it, "1").empty());
    CHECK(di::resolved_member(it, "1").empty());
    CHECK(di::resolved_channel(it, "1").empty());
    CHECK(di::resolved_role(it, "1").empty());
    CHECK(di::resolved_attachment(it, "1").empty());
}

// ---------------------------------------------------------------------------
// Throw-free access on wrong-typed untrusted JSON (FIX: json_access contract)
// ---------------------------------------------------------------------------

TEST_CASE("command_options tolerates a wrong-typed option type without throwing") {
    // "type" is the string "1", not the integer 1: the walker must treat it as
    // a plain (non-subcommand) option list instead of throwing.
    const json interaction = json{
        {"type", 2},
        {"data",
         {{"name", "config"},
          {"options",
           json::array({json{{"name", "text"}, {"type", "1"}, {"value", "hello"}}})}}}};
    json options{};
    CHECK_NOTHROW(options = di::command_options(interaction));
    REQUIRE(options.is_array());
    CHECK(options.size() == 1);
    CHECK(di::option_string(interaction, "text").value_or("") == "hello");
}

TEST_CASE("option lookups skip a numeric option name without throwing") {
    const json interaction = json{
        {"type", 2},
        {"data",
         {{"name", "config"},
          {"options",
           json::array({json{{"name", 42}, {"type", 3}, {"value", "bogus"}},
                        json{{"name", "real"}, {"type", 3}, {"value", "ok"}}})}}}};
    std::optional<std::string> value{};
    CHECK_NOTHROW(value = di::option_string(interaction, "real"));
    REQUIRE(value.has_value());
    CHECK(value.value() == "ok");
    CHECK_NOTHROW(di::option_string(interaction, "42"));
    CHECK_FALSE(di::option_string(interaction, "42").has_value());
}

TEST_CASE("option_id returns nullopt on a wrong-typed option type without throwing") {
    // "type" is the string "6" — not an integer snowflake type code.
    const json interaction = json{
        {"type", 2},
        {"data",
         {{"name", "config"},
          {"options",
           json::array({json{{"name", "who"}, {"type", "6"}, {"value", "123"}}})}}}};
    std::optional<std::string> id{};
    CHECK_NOTHROW(id = di::option_id(interaction, "who"));
    CHECK_FALSE(id.has_value());
}
