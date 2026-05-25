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

inline ModuleRoute route_from_manifest_entry(const json& manifest,
                                             const std::string& kind,
                                             const std::string& route) {
    const json& routes = manifest.at("routes").at(kind);
    const auto match = routes.find(route);
    if (match == routes.end() || !match->is_string()) {
        throw std::runtime_error("module route not found: " + route);
    }

    return {
        manifest.value("name", ""),
        route,
        match->get<std::string>(),
        manifest.value("error_mapper", json::object()).value("lambda", ""),
    };
}

}  // namespace discord_interactions
