#pragma once

// Fluent, limit-enforcing Discord embed builder (discord.js EmbedBuilder parity).
//
// Every string setter clamps its argument through `safe_truncate` with the
// matching per-field limit from `limits.hpp`, so an embed can never carry a
// value that Discord would reject for length. `field(...)` silently drops any
// field past the 25-field cap. `build()` performs the combined 6000-character
// check across all length-counted fields (title, description, field
// names/values, footer text, author name) and throws a validation
// `ModuleError` (code "embed_too_large") when it overflows.
//
// Header-only (AD-2), namespace discord_interactions, consumed identically by
// unit tests and Lambdas. Only depends on nlohmann/json plus the framework's
// limits.hpp and errors.hpp.

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "discord_interactions/errors.hpp"
#include "discord_interactions/limits.hpp"

namespace discord_interactions {

class EmbedBuilder {
public:
    EmbedBuilder& title(const std::string& text) {
        embed_["title"] = safe_truncate(text, limits::embed_title);
        return *this;
    }

    EmbedBuilder& description(const std::string& text) {
        embed_["description"] = safe_truncate(text, limits::embed_description);
        return *this;
    }

    EmbedBuilder& url(const std::string& value) {
        embed_["url"] = value;
        return *this;
    }

    EmbedBuilder& color(uint32_t rgb) {
        embed_["color"] = rgb;
        return *this;
    }

    EmbedBuilder& timestamp_iso8601(const std::string& value) {
        embed_["timestamp"] = value;
        return *this;
    }

    EmbedBuilder& footer(const std::string& text,
                         const std::string& icon_url = "") {
        json footer = json::object();
        footer["text"] = safe_truncate(text, limits::embed_footer);
        if (!icon_url.empty()) {
            footer["icon_url"] = icon_url;
        }
        embed_["footer"] = std::move(footer);
        return *this;
    }

    EmbedBuilder& author(const std::string& name, const std::string& url = "",
                         const std::string& icon_url = "") {
        json author = json::object();
        author["name"] = safe_truncate(name, limits::embed_author);
        if (!url.empty()) {
            author["url"] = url;
        }
        if (!icon_url.empty()) {
            author["icon_url"] = icon_url;
        }
        embed_["author"] = std::move(author);
        return *this;
    }

    EmbedBuilder& thumbnail(const std::string& url) {
        embed_["thumbnail"] = json{{"url", url}};
        return *this;
    }

    EmbedBuilder& image(const std::string& url) {
        embed_["image"] = json{{"url", url}};
        return *this;
    }

    EmbedBuilder& field(const std::string& name, const std::string& value,
                        bool inline_field = false) {
        if (fields_.size() >= limits::embed_fields) {
            return *this;  // Silently drop fields past the 25-field cap.
        }
        json f = json::object();
        f["name"] = safe_truncate(name, limits::embed_field_name);
        f["value"] = safe_truncate(value, limits::embed_field_value);
        f["inline"] = inline_field;
        fields_.push_back(std::move(f));
        return *this;
    }

    json build() const {
        json out = embed_;
        if (!fields_.empty()) {
            out["fields"] = fields_;
        }

        std::size_t total = 0;
        total += string_length(out, "title");
        total += string_length(out, "description");
        if (out.contains("footer") && out["footer"].contains("text")) {
            total += out["footer"]["text"].get<std::string>().size();
        }
        if (out.contains("author") && out["author"].contains("name")) {
            total += out["author"]["name"].get<std::string>().size();
        }
        for (const auto& f : fields_) {
            total += f["name"].get<std::string>().size();
            total += f["value"].get<std::string>().size();
        }

        if (total > limits::embed_total) {
            throw MODULE_ERROR("embed_too_large", ErrorCategory::validation,
                               "embed exceeds the combined character limit");
        }

        return out;
    }

private:
    static std::size_t string_length(const json& obj, const char* key) {
        if (obj.contains(key) && obj[key].is_string()) {
            return obj[key].get<std::string>().size();
        }
        return 0;
    }

    json embed_ = json::object();
    json fields_ = json::array();
};

}  // namespace discord_interactions
