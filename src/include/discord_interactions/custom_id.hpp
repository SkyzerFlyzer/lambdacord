#pragma once

// Structured custom_id state codec (plan T1.7).
//
// Serverless contract — READ THIS BEFORE STORING STATE IN A custom_id.
// In this architecture there is no in-memory session between interactions:
// each Discord interaction lands on a fresh (or unrelated warm) Lambda with no
// shared memory. A component's `custom_id` is therefore the framework's ONLY
// free state channel — the sole piece of state that Discord round-trips back to
// us when a button/select is used. It is delivered verbatim on the follow-up
// interaction and costs nothing to carry.
//
// That channel is small and its size is a HARD architectural boundary: Discord
// caps a custom_id at limits::custom_id (100 bytes). This codec enforces that
// budget at encode time. State that does not fit in 100 bytes does NOT belong
// in a custom_id — it belongs in module-owned durable storage (e.g. DynamoDB),
// keyed by a compact id that DOES fit here. Do not try to smuggle large state
// through the custom_id; encode a key and look the rest up.
//
// Wire format: a ':'-separated list, `prefix[:arg]*`. The prefix routes the
// component (see routing.hpp / the message-component handler); the args are the
// per-component state. Because ':' is the delimiter, neither the prefix nor any
// arg may contain ':'. Empty args are legal and preserved ("p::x" -> {"", "x"}).
//
// Header-only, no third-party runtime dependencies beyond the C++ standard
// library (AD-2/AD-7). Consumed identically by unit tests and Lambdas.

#include <string>
#include <vector>

#include "discord_interactions/errors.hpp"
#include "discord_interactions/limits.hpp"

namespace discord_interactions {

// Encodes `prefix` and `args` into a `prefix:arg0:arg1...` custom_id.
//
// Throws ModuleError(category=validation) when:
//   - `prefix` is empty or contains ':',
//   - any element of `args` contains ':',
//   - the encoded length exceeds limits::custom_id (100 bytes).
inline std::string encode_custom_id(const std::string& prefix,
                                    const std::vector<std::string>& args) {
    if (prefix.empty()) {
        throw MODULE_ERROR("custom_id_invalid", ErrorCategory::validation,
                           "custom_id prefix must not be empty");
    }
    if (prefix.find(':') != std::string::npos) {
        throw MODULE_ERROR("custom_id_invalid", ErrorCategory::validation,
                           "custom_id prefix must not contain ':'");
    }

    std::string encoded = prefix;
    for (const std::string& arg : args) {
        if (arg.find(':') != std::string::npos) {
            throw MODULE_ERROR("custom_id_invalid", ErrorCategory::validation,
                               "custom_id arg must not contain ':'");
        }
        encoded += ':';
        encoded += arg;
    }

    if (encoded.size() > limits::custom_id) {
        throw MODULE_ERROR(
            "custom_id_too_long", ErrorCategory::validation,
            "custom_id exceeds Discord's " + std::to_string(limits::custom_id) +
                "-character limit; move larger state into module-owned storage");
    }

    return encoded;
}

// Parsed view of a custom_id: a routing prefix plus its state args.
struct CustomId {
    std::string prefix{};
    std::vector<std::string> args{};
};

// Parses a `prefix[:arg]*` custom_id. "prefix" alone yields {prefix, {}}.
// Empty args are preserved. Throws ModuleError(category=validation) when the
// prefix is empty (raw is empty, or raw begins with ':').
inline CustomId parse_custom_id(const std::string& raw) {
    CustomId result{};

    std::string::size_type start = 0;
    std::string::size_type sep = raw.find(':');
    result.prefix = raw.substr(0, sep);
    if (result.prefix.empty()) {
        throw MODULE_ERROR("custom_id_invalid", ErrorCategory::validation,
                           "custom_id prefix must not be empty");
    }

    while (sep != std::string::npos) {
        start = sep + 1;
        sep = raw.find(':', start);
        result.args.push_back(raw.substr(start, sep - start));
    }

    return result;
}

}  // namespace discord_interactions
