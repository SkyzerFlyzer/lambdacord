#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include "discord_interactions/autocomplete.hpp"

using nlohmann::json;
namespace di = discord_interactions;

namespace {

// Autocomplete interaction (type 4) with a top-level focused STRING option.
json top_level_autocomplete() {
    return json{
        {"type", 4},
        {"data",
         {{"id", "cmd1"},
          {"name", "search"},
          {"type", 1},
          {"options",
           json::array(
               {json{{"name", "term"}, {"type", 3}, {"value", "he"}, {"focused", true}},
                json{{"name", "limit"}, {"type", 4}, {"value", 5}}})}}}};
}

// Autocomplete interaction with the focused option nested under a subcommand,
// and its partial value delivered as a JSON number (must stringify).
json nested_autocomplete_numeric() {
    return json{
        {"type", 4},
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
                         {json{{"name", "minutes"},
                               {"type", 10},
                               {"value", 12},
                               {"focused", true}}})}}})}}})}}}};
}

// No focused option anywhere.
json autocomplete_no_focus() {
    return json{
        {"type", 4},
        {"data",
         {{"name", "search"},
          {"type", 1},
          {"options",
           json::array({json{{"name", "term"}, {"type", 3}, {"value", "hi"}}})}}}};
}

}  // namespace

TEST_CASE("choice(string) exact JSON") {
    const json c = di::choice("Apple", std::string("apple"));
    CHECK(c == json{{"name", "Apple"}, {"value", "apple"}});
}

TEST_CASE("choice(int64) exact JSON") {
    const json c = di::choice("Twelve", int64_t(12));
    CHECK(c == json{{"name", "Twelve"}, {"value", 12}});
    CHECK(c.at("value").is_number_integer());
}

TEST_CASE("choice(double) exact JSON") {
    const json c = di::choice("Half", 0.5);
    CHECK(c.at("name") == "Half");
    CHECK(c.at("value").is_number_float());
    CHECK(c.at("value").get<double>() == doctest::Approx(0.5));
}

TEST_CASE("autocomplete_response wraps choices with type 8") {
    json choices = json::array({di::choice("A", std::string("a"))});
    const json resp = di::autocomplete_response(choices);
    CHECK(resp.at("type") == 8);
    REQUIRE(resp.at("data").at("choices").is_array());
    CHECK(resp.at("data").at("choices").size() == 1);
    CHECK(resp.at("data").at("choices").at(0).at("name") == "A");
}

TEST_CASE("autocomplete_response caps at 25 choices, first 25 kept") {
    json choices = json::array();
    for (int i = 0; i < 40; ++i) {
        choices.push_back(di::choice("c" + std::to_string(i), int64_t(i)));
    }
    const json resp = di::autocomplete_response(choices);
    const json& out = resp.at("data").at("choices");
    CHECK(out.size() == 25);
    CHECK(out.at(0).at("name") == "c0");
    CHECK(out.at(24).at("name") == "c24");
}

TEST_CASE("autocomplete_response truncates names to 100 chars") {
    const std::string long_name(300, 'x');
    json choices = json::array({di::choice(long_name, std::string("v"))});
    const json resp = di::autocomplete_response(choices);
    const std::string out = resp.at("data").at("choices").at(0).at("name");
    CHECK(out.size() <= 100);
    CHECK(out.size() < long_name.size());
}

TEST_CASE("autocomplete_response passes a 100-char string value through unchanged") {
    const std::string value(100, 'v');
    json choices = json::array({di::choice("name", value)});
    const json resp = di::autocomplete_response(choices);
    const std::string out = resp.at("data").at("choices").at(0).at("value");
    CHECK(out == value);
    CHECK(out.size() == 100);
}

TEST_CASE("autocomplete_response clamps an oversized ASCII string value to 100 bytes with no ellipsis") {
    const std::string value(300, 'v');
    json choices = json::array({di::choice("name", value)});
    const json resp = di::autocomplete_response(choices);
    const std::string out = resp.at("data").at("choices").at(0).at("value");
    CHECK(out.size() == 100);
    // Machine-facing identifier: no ellipsis appended.
    CHECK(out == std::string(100, 'v'));
}

TEST_CASE("autocomplete_response clamps a multibyte string value on a codepoint boundary") {
    // 99 ASCII bytes then a 2-byte é (U+00E9): the byte at index 100 would land
    // mid-sequence, so the cut must retreat to 99 bytes to stay valid UTF-8.
    std::string value(99, 'a');
    value += "\xC3\xA9";  // é, 2 bytes -> total 101 bytes
    value += "tail";
    json choices = json::array({di::choice("name", value)});
    const json resp = di::autocomplete_response(choices);
    const std::string out = resp.at("data").at("choices").at(0).at("value");
    CHECK(out.size() <= 100);
    // Never split the multibyte sequence: the trailing é must be dropped whole.
    CHECK(out == std::string(99, 'a'));
    // The result must be valid UTF-8 (nlohmann::json only serializes valid UTF-8).
    CHECK_NOTHROW(json({{"v", out}}).dump());
}

TEST_CASE("autocomplete_response leaves numeric choice values untouched") {
    json choices = json::array(
        {di::choice("i", int64_t(123456789)), di::choice("d", 0.5)});
    const json resp = di::autocomplete_response(choices);
    const json& out = resp.at("data").at("choices");
    CHECK(out.at(0).at("value") == 123456789);
    CHECK(out.at(0).at("value").is_number_integer());
    CHECK(out.at(1).at("value").get<double>() == doctest::Approx(0.5));
    CHECK(out.at(1).at("value").is_number_float());
}

TEST_CASE("filter_choices empty query returns all unchanged") {
    json choices = json::array(
        {di::choice("Banana", std::string("b")), di::choice("Apple", std::string("a"))});
    const json out = di::filter_choices(choices, "");
    CHECK(out == choices);
}

TEST_CASE("filter_choices prefix matches before substring, stable within class") {
    // Craft a fixture where a substring match ("Pineapple") precedes a prefix
    // match ("Apple") in input order. Prefix must still come first in output.
    json choices = json::array({
        di::choice("Pineapple", std::string("pine")),   // substring of "ap"
        di::choice("Apricot", std::string("apr")),        // prefix "ap"
        di::choice("Cherry", std::string("cherry")),     // no "ap" match
        di::choice("Apple", std::string("apple")),        // prefix "ap"
        di::choice("Snapple", std::string("snap")),       // substring "ap"
    });
    const json out = di::filter_choices(choices, "ap");
    REQUIRE(out.size() == 4);
    // Prefix class first, in input order: Apricot, Apple.
    CHECK(out.at(0).at("name") == "Apricot");
    CHECK(out.at(1).at("name") == "Apple");
    // Substring class next, in input order: Pineapple, Snapple.
    CHECK(out.at(2).at("name") == "Pineapple");
    CHECK(out.at(3).at("name") == "Snapple");
}

TEST_CASE("filter_choices is case-insensitive") {
    json choices = json::array(
        {di::choice("APPLE", std::string("a")), di::choice("banana", std::string("b"))});
    const json out = di::filter_choices(choices, "aPp");
    REQUIRE(out.size() == 1);
    CHECK(out.at(0).at("name") == "APPLE");
}

TEST_CASE("focused_option_name/value from top-level focused option") {
    const auto name = di::focused_option_name(top_level_autocomplete());
    const auto value = di::focused_option_value(top_level_autocomplete());
    REQUIRE(name.has_value());
    CHECK(name.value() == "term");
    REQUIRE(value.has_value());
    CHECK(value.value() == "he");
}

TEST_CASE("focused_option from nested subcommand, numeric value stringified") {
    const auto name = di::focused_option_name(nested_autocomplete_numeric());
    const auto value = di::focused_option_value(nested_autocomplete_numeric());
    REQUIRE(name.has_value());
    CHECK(name.value() == "minutes");
    REQUIRE(value.has_value());
    CHECK(value.value() == "12");
}

TEST_CASE("no focused option yields nullopt for both getters") {
    CHECK_FALSE(di::focused_option_name(autocomplete_no_focus()).has_value());
    CHECK_FALSE(di::focused_option_value(autocomplete_no_focus()).has_value());
    // Interaction with no data at all.
    CHECK_FALSE(di::focused_option_name(json{{"type", 4}}).has_value());
    CHECK_FALSE(di::focused_option_value(json{{"type", 4}}).has_value());
}

// ---------------------------------------------------------------------------
// Throw-free focused walker on wrong-typed untrusted JSON
// ---------------------------------------------------------------------------

TEST_CASE("focused walker skips an integer focused flag without throwing") {
    // "focused" is the integer 1, not a boolean: the walker must skip it
    // instead of throwing a nlohmann type_error.
    const json interaction = json{
        {"type", 4},
        {"data",
         {{"name", "search"},
          {"options",
           json::array({json{{"name", "term"}, {"type", 3}, {"focused", 1}}})}}}};
    std::optional<std::string> name{};
    CHECK_NOTHROW(name = di::focused_option_name(interaction));
    CHECK_FALSE(name.has_value());
    CHECK_NOTHROW(di::focused_option_value(interaction));
}

TEST_CASE("focused walker tolerates a string type code and still finds the focused option") {
    const json interaction = json{
        {"type", 4},
        {"data",
         {{"name", "search"},
          {"options",
           json::array({json{{"name", "grp"}, {"type", "1"},
                             {"options", json::array()}},
                        json{{"name", "term"}, {"type", 3}, {"value", "pa"},
                             {"focused", true}}})}}}};
    std::optional<std::string> name{};
    CHECK_NOTHROW(name = di::focused_option_name(interaction));
    REQUIRE(name.has_value());
    CHECK(name.value() == "term");
    CHECK(di::focused_option_value(interaction).value_or("") == "pa");
}
