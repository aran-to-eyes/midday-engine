// core/base/error.h — the engine's structured error envelope: ONE definition
// of code/message/details for the whole tree. cli/envelope.h wraps this type
// into the CLI JSON envelope (formats/cli_envelope.schema.json's `error`
// object); journals and verbs reuse it as-is. Errors are values, returned —
// never thrown across subsystem boundaries.

#pragma once

#include "core/base/json.h"

#include <optional>
#include <string>

namespace midday::base {

struct Error {
    std::string code;              // stable dotted identifier, e.g. "json.parse", "selftest.failed"
    std::string message;           // one-line human summary
    Json details = Json::object(); // structured, code-specific diagnostics

    // {"code":...,"message":...[,"details":{...}]} — details omitted when empty.
    [[nodiscard]] Json to_json() const;

    // Strict inverse of to_json: exactly the keys above, code non-empty,
    // details (if present) an object. Anything else -> nullopt.
    static std::optional<Error> from_json(const Json& json);
};

// Lift a strict-parse diagnostic into the envelope: code "json.parse",
// message "origin:line:col: ...", details {file,line,col,offset}.
Error to_error(const JsonParseError& parse_error);

} // namespace midday::base
