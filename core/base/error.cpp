#include "core/base/error.h"

namespace midday::base {

Json Error::to_json() const {
    Json out = Json::object();
    out.set("code", code);
    out.set("message", message);
    if (details.is_object() && !details.items().empty())
        out.set("details", details);
    return out;
}

std::optional<Error> Error::from_json(const Json& json) {
    if (!json.is_object())
        return std::nullopt;
    const Json* code = json.find("code");
    const Json* message = json.find("message");
    if (code == nullptr || !code->is_string() || code->as_string().empty())
        return std::nullopt;
    if (message == nullptr || !message->is_string())
        return std::nullopt;

    Error error{.code = code->as_string(), .message = message->as_string()};
    for (const auto& [key, value] : json.items()) {
        if (key == "code" || key == "message")
            continue;
        if (key == "details" && value.is_object()) {
            error.details = value;
            continue;
        }
        return std::nullopt; // unknown key or non-object details: not an Error
    }
    return error;
}

Error to_error(const JsonParseError& parse_error) {
    Error error{.code = "json.parse", .message = parse_error.to_string()};
    error.details.set("file", parse_error.origin);
    error.details.set("line", parse_error.line);
    error.details.set("col", parse_error.col);
    error.details.set("offset", static_cast<std::int64_t>(parse_error.offset));
    return error;
}

} // namespace midday::base
