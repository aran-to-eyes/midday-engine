#include "core/expr/diag.h"

#include <cstdint>

namespace midday::expr {

std::string Diag::to_string() const {
    return origin + ":" + std::to_string(line) + ":" + std::to_string(col) + ": " + message;
}

base::Error Diag::to_error() const {
    base::Error error{.code = code, .message = to_string()};
    error.details.set("file", origin);
    error.details.set("line", line);
    error.details.set("col", col);
    error.details.set("offset", static_cast<std::int64_t>(offset));
    return error;
}

} // namespace midday::expr
