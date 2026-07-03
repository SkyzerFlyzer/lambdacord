#pragma once

#include <nlohmann/json.hpp>

#include <stdexcept>
#include <string>

namespace discord_interactions {

using json = nlohmann::json;

struct ModuleRoute {
    std::string module{};
    std::string route{};
    std::string function_name{};
    std::string error_mapper_function_name{};
};

// Accepts both manifest route value forms (T3.2):
//   "route": "discord-cmd-x"                                  (plain string)
//   "route": {"lambda": "discord-cmd-x", "ephemeral_defer": true}   (object)
// The object form requires a non-empty string "lambda" key; anything else
// (missing route, wrong type, object without "lambda") throws.
inline ModuleRoute route_from_manifest_entry(const json& manifest,
                                             const std::string& kind,
                                             const std::string& route) {
    const json& routes = manifest.at("routes").at(kind);
    const auto match = routes.find(route);

    std::string function_name{};
    if (match != routes.end()) {
        if (match->is_string()) {
            function_name = match->get<std::string>();
        } else if (match->is_object()) {
            const auto lambda = match->find("lambda");
            if (lambda != match->end() && lambda->is_string()) {
                function_name = lambda->get<std::string>();
            }
        }
    }
    if (function_name.empty()) {
        throw std::runtime_error("module route not found: " + route);
    }

    return {
        manifest.value("name", ""),
        route,
        function_name,
        manifest.value("error_mapper", json::object()).value("lambda", ""),
    };
}

}  // namespace discord_interactions
