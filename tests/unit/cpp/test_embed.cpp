#include <doctest/doctest.h>

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "discord_interactions/embed.hpp"
#include "discord_interactions/errors.hpp"
#include "discord_interactions/limits.hpp"

using discord_interactions::EmbedBuilder;
using discord_interactions::ErrorCategory;
using discord_interactions::ModuleError;
using json = nlohmann::json;
namespace limits = discord_interactions::limits;

TEST_CASE("empty builder produces a minimal empty object") {
    const json out = EmbedBuilder{}.build();
    CHECK(out.is_object());
    CHECK(out.empty());
}

TEST_CASE("title setter is reflected and truncated to the embed_title limit") {
    const json out = EmbedBuilder{}.title("Hello").build();
    CHECK(out.at("title") == "Hello");

    const std::string long_title(limits::embed_title + 50, 'a');
    const json truncated = EmbedBuilder{}.title(long_title).build();
    CHECK(truncated.at("title").get<std::string>().size() <= limits::embed_title);
}

TEST_CASE("description setter is reflected and truncated") {
    const json out = EmbedBuilder{}.description("body text").build();
    CHECK(out.at("description") == "body text");

    const std::string long_desc(limits::embed_description + 100, 'x');
    const json truncated = EmbedBuilder{}.description(long_desc).build();
    CHECK(truncated.at("description").get<std::string>().size() <=
          limits::embed_description);
}

TEST_CASE("url and color setters are reflected") {
    const json out =
        EmbedBuilder{}.url("https://example.com").color(0x5865F2).build();
    CHECK(out.at("url") == "https://example.com");
    CHECK(out.at("color") == 0x5865F2);
}

TEST_CASE("timestamp setter is reflected") {
    const json out =
        EmbedBuilder{}.timestamp_iso8601("2026-07-02T00:00:00Z").build();
    CHECK(out.at("timestamp") == "2026-07-02T00:00:00Z");
}

TEST_CASE("footer emits text and only emits icon_url when non-empty") {
    const json no_icon = EmbedBuilder{}.footer("footer text").build();
    CHECK(no_icon.at("footer").at("text") == "footer text");
    CHECK_FALSE(no_icon.at("footer").contains("icon_url"));

    const json with_icon =
        EmbedBuilder{}.footer("footer text", "https://cdn/i.png").build();
    CHECK(with_icon.at("footer").at("icon_url") == "https://cdn/i.png");
}

TEST_CASE("footer text is truncated to the embed_footer limit") {
    const std::string long_text(limits::embed_footer + 40, 'f');
    const json out = EmbedBuilder{}.footer(long_text).build();
    CHECK(out.at("footer").at("text").get<std::string>().size() <=
          limits::embed_footer);
}

TEST_CASE("author emits name and only emits url/icon_url when non-empty") {
    const json bare = EmbedBuilder{}.author("Author Name").build();
    CHECK(bare.at("author").at("name") == "Author Name");
    CHECK_FALSE(bare.at("author").contains("url"));
    CHECK_FALSE(bare.at("author").contains("icon_url"));

    const json full = EmbedBuilder{}
                          .author("Author Name", "https://example.com",
                                  "https://cdn/a.png")
                          .build();
    CHECK(full.at("author").at("url") == "https://example.com");
    CHECK(full.at("author").at("icon_url") == "https://cdn/a.png");
}

TEST_CASE("author name is truncated to the embed_author limit") {
    const std::string long_name(limits::embed_author + 20, 'n');
    const json out = EmbedBuilder{}.author(long_name).build();
    CHECK(out.at("author").at("name").get<std::string>().size() <=
          limits::embed_author);
}

TEST_CASE("thumbnail and image emit nested url objects") {
    const json out = EmbedBuilder{}
                         .thumbnail("https://cdn/t.png")
                         .image("https://cdn/i.png")
                         .build();
    CHECK(out.at("thumbnail").at("url") == "https://cdn/t.png");
    CHECK(out.at("image").at("url") == "https://cdn/i.png");
}

TEST_CASE("field emits name, value and inline flag; values truncated") {
    const json out = EmbedBuilder{}.field("Name", "Value", true).build();
    REQUIRE(out.at("fields").is_array());
    REQUIRE(out.at("fields").size() == 1);
    CHECK(out.at("fields")[0].at("name") == "Name");
    CHECK(out.at("fields")[0].at("value") == "Value");
    CHECK(out.at("fields")[0].at("inline") == true);

    const std::string long_name(limits::embed_field_name + 10, 'n');
    const std::string long_value(limits::embed_field_value + 10, 'v');
    const json truncated = EmbedBuilder{}.field(long_name, long_value).build();
    CHECK(truncated.at("fields")[0].at("name").get<std::string>().size() <=
          limits::embed_field_name);
    CHECK(truncated.at("fields")[0].at("value").get<std::string>().size() <=
          limits::embed_field_value);
    CHECK(truncated.at("fields")[0].at("inline") == false);
}

TEST_CASE("field cap silently drops the 26th and beyond") {
    EmbedBuilder builder{};
    for (int i = 0; i < 30; ++i) {
        builder.field("n" + std::to_string(i), "v");
    }
    const json out = builder.build();
    CHECK(out.at("fields").size() == limits::embed_fields);
    CHECK(out.at("fields").size() == 25);
    // The kept fields are the first 25.
    CHECK(out.at("fields")[0].at("name") == "n0");
    CHECK(out.at("fields")[24].at("name") == "n24");
}

TEST_CASE("fluent chaining returns the same builder object") {
    EmbedBuilder builder{};
    EmbedBuilder& returned = builder.title("t");
    CHECK(&returned == &builder);
    EmbedBuilder& returned2 = builder.description("d").color(1).field("a", "b");
    CHECK(&returned2 == &builder);
}

TEST_CASE("total over embed_total throws embed_too_large validation error") {
    EmbedBuilder builder{};
    // 25 fields of ~250-char values easily exceed 6000 combined characters.
    const std::string big(250, 'z');
    for (int i = 0; i < 25; ++i) {
        builder.field("field-name", big);
    }
    bool threw = false;
    try {
        builder.build();
    } catch (const ModuleError& e) {
        threw = true;
        CHECK(e.code == "embed_too_large");
        CHECK(e.category == ErrorCategory::validation);
    }
    CHECK(threw);
}

TEST_CASE("total at or under embed_total does not throw") {
    EmbedBuilder builder{};
    builder.title(std::string(100, 'a')).description(std::string(100, 'b'));
    CHECK_NOTHROW(builder.build());
}
