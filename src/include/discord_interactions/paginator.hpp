#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <stdexcept>
#include <string>

#include "discord_interactions/custom_id.hpp"

namespace discord_interactions {

using json = nlohmann::json;

inline json paginator_button(const std::string& custom_id,
                             const std::string& label,
                             bool disabled) {
    return {
        {"type", 2},
        {"style", 2},
        {"label", label},
        {"custom_id", custom_id},
        {"disabled", disabled},
    };
}

inline json paginator_row(const std::string& prefix, size_t page, size_t total_pages) {
    const size_t last_page = total_pages == 0 ? 0 : total_pages - 1;
    const size_t prev_page = page == 0 ? 0 : page - 1;
    const size_t next_page = page >= last_page ? last_page : page + 1;
    return json::array({{{"type", 1},
                         {"components",
                          json::array({
                              paginator_button(encode_custom_id(prefix, {std::to_string(prev_page)}),
                                               "Previous",
                                               page == 0),
                              paginator_button(encode_custom_id(prefix, {std::to_string(next_page)}),
                                               "Next",
                                               page >= last_page),
                          })}}});
}

inline size_t parse_page_after_prefix(const std::string& custom_id,
                                      const std::string& expected_prefix) {
    const CustomId parsed = parse_custom_id(custom_id);
    if (parsed.prefix != expected_prefix || parsed.args.empty()) {
        throw std::runtime_error("Unexpected paginator custom_id");
    }
    return static_cast<size_t>(std::stoul(parsed.args.front()));
}

}  // namespace discord_interactions
