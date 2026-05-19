#pragma once

#include <nlohmann/json.hpp>

#include <exception>
#include <string>

namespace discord_interactions {

using json = nlohmann::json;

enum class ErrorCategory {
    validation,
    auth,
    upstream,
    storage,
    configuration,
    rate_limited,
    internal,
};

inline const char* error_category_name(ErrorCategory category) {
    switch (category) {
    case ErrorCategory::validation:
        return "validation";
    case ErrorCategory::auth:
        return "auth";
    case ErrorCategory::upstream:
        return "upstream";
    case ErrorCategory::storage:
        return "storage";
    case ErrorCategory::configuration:
        return "configuration";
    case ErrorCategory::rate_limited:
        return "rate_limited";
    case ErrorCategory::internal:
        return "internal";
    }
    return "internal";
}

struct ModuleError : std::exception {
    std::string code{};
    ErrorCategory category = ErrorCategory::internal;
    std::string internal_message{};
    std::string file{};
    int line = 0;
    std::string function{};
    json safe_context = json::object();
    json debug_context = json::object();

    const char* what() const noexcept override {
        return internal_message.c_str();
    }

    static ModuleError make(std::string error_code,
                            ErrorCategory error_category,
                            std::string message,
                            std::string source_file,
                            int source_line,
                            std::string source_function,
                            json safe = json::object(),
                            json debug = json::object()) {
        ModuleError error{};
        error.code = std::move(error_code);
        error.category = error_category;
        error.internal_message = std::move(message);
        error.file = std::move(source_file);
        error.line = source_line;
        error.function = std::move(source_function);
        error.safe_context = std::move(safe);
        error.debug_context = std::move(debug);
        return error;
    }
};

inline json to_json(const ModuleError& error) {
    return {
        {"code", error.code},
        {"category", error_category_name(error.category)},
        {"internal_message", error.internal_message},
        {"function", error.function},
        {"file", error.file},
        {"line", error.line},
        {"safe_context", error.safe_context},
        {"debug_context", error.debug_context},
    };
}

inline json error_mapper_request(const std::string& module_name,
                                 const std::string& route,
                                 const json& interaction,
                                 const ModuleError& error) {
    return {
        {"module", module_name},
        {"route", route},
        {"interaction", interaction},
        {"error", to_json(error)},
    };
}

}  // namespace discord_interactions

#define MODULE_ERROR(code, category, message) \
    ::discord_interactions::ModuleError::make((code), (category), (message), __FILE__, __LINE__, __func__)
