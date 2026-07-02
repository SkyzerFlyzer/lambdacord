#pragma once

// Throw-free typed access into untrusted interaction JSON.
//
// nlohmann's `json::value(key, default)` THROWS a type_error when the key is
// PRESENT but holds the wrong type — a hostile or malformed interaction
// payload with `"type": "1"` (string) would crash any helper documented as
// non-throwing. Every builder/walker in this framework that reads a field off
// wire-delivered interaction JSON must go through the accessors here instead:
// they mirror the find()+is_*() idiom premium.hpp and modal.hpp already use,
// and NEVER throw on any input shape.
//
// - get_if<T>(node, key): std::optional<T>. nullopt when `node` is not an
//   object, the key is absent, or the value's JSON type does not match T
//   (bool -> is_boolean, std::string -> is_string, integral -> is_number_integer,
//   floating point -> is_number). Use `.value_or(default)` for the old
//   `value(key, default)` call shape.
// - get_array_if(node, key): pointer to the array under `key`, or nullptr when
//   `node` is not an object, the key is absent, or the value is not an array.
//   A pointer (not a copy) so recursive walkers can descend without copying.
//
// Header-only, no third-party runtime dependencies beyond nlohmann/json
// (AD-2/AD-7). Consumed identically by unit tests and Lambdas.

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <type_traits>

namespace discord_interactions {

using json = nlohmann::json;

// Throw-free typed field read; see the header comment for the exact per-type
// matching rules. Returns nullopt instead of throwing on every mismatch.
template <typename T>
inline std::optional<T> get_if(const json& node, const char* key) {
    if (!node.is_object()) {
        return std::nullopt;
    }
    const auto it = node.find(key);
    if (it == node.end()) {
        return std::nullopt;
    }
    if constexpr (std::is_same_v<T, bool>) {
        if (!it->is_boolean()) {
            return std::nullopt;
        }
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (!it->is_string()) {
            return std::nullopt;
        }
    } else if constexpr (std::is_floating_point_v<T>) {
        // Any JSON number (integer or float) converts losslessly enough for a
        // floating-point read; a boolean or string is a mismatch.
        if (!it->is_number()) {
            return std::nullopt;
        }
    } else if constexpr (std::is_integral_v<T>) {
        // is_number_integer() excludes floats so 3.5 is a mismatch, never a
        // silent truncation (mirrors options.hpp's option_int contract).
        if (!it->is_number_integer()) {
            return std::nullopt;
        }
    } else {
        static_assert(std::is_same_v<T, bool> || std::is_same_v<T, std::string> ||
                          std::is_arithmetic_v<T>,
                      "get_if supports bool, std::string, and arithmetic types");
    }
    return it->get<T>();
}

// Throw-free array field read: a pointer to the array under `key`, or nullptr
// when `node` is not an object, the key is absent, or the value is not an
// array. Never throws.
inline const json* get_array_if(const json& node, const char* key) {
    if (!node.is_object()) {
        return nullptr;
    }
    const auto it = node.find(key);
    if (it == node.end() || !it->is_array()) {
        return nullptr;
    }
    return &*it;
}

}  // namespace discord_interactions
