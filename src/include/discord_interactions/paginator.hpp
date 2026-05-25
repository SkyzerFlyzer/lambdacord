#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>

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
    return json::array({{{"type", 1},
                         {"components",
                          json::array({
                              paginator_button(prefix + ":" + std::to_string(page == 0 ? 0 : page - 1),
                                               "Previous",
                                               page == 0),
                              paginator_button(prefix + ":" + std::to_string(page >= last_page ? last_page : page + 1),
                                               "Next",
                                               page >= last_page),
                          })}}});
}

inline size_t parse_page_after_prefix(const std::string& custom_id,
                                      const std::string& expected_prefix) {
    const std::string marker = expected_prefix + ":";
    if (custom_id.rfind(marker, 0) != 0) {
        throw std::runtime_error("Unexpected paginator custom_id");
    }
    return static_cast<size_t>(std::stoul(custom_id.substr(marker.size())));
}

}  // namespace discord_interactions
