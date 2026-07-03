// core/expr/diag.h — structured compile-time diagnostics for the expression
// language, following the base::Json parse conventions (origin:line:col,
// 1-based, col counts bytes since the last newline).
//
// Codes (stable dotted identifiers, all under "expr."):
//   expr.parse            — syntax error (unexpected token, bad literal, ...)
//   expr.side_effect      — side-effecting/stateful syntax REJECTED AT PARSE:
//                           `=`, `+=` `-=` `*=` `/=` `%=`, `++`/`--`, `;`,
//                           and reserved statement words (if/while/for/let/...)
//   expr.unknown_variable — identifier path resolves to no declared variable
//   expr.unknown_function — call target is not in the function inventory
//   expr.arity            — wrong number of call arguments
//   expr.type             — static type error (operands, args, branches, members)
//   expr.too_complex      — program exceeds the compiled-form limits
//
// EVERY compile-time expr.* diagnostic is VALIDATION-class: the CLI maps it
// to exit code 3 (spec section 9), pinned here as kCompileDiagExitCode so the
// mapping is asserted at the Error level before the CLI wiring lands
// (m0-api-json / validate verbs). Runtime eval statuses (program.h) are the
// FAILURE class (exit 1) — a filter that divides by zero at runtime is a
// failed run, not an invalid file.

#pragma once

#include "core/base/error.h"

#include <cstddef>
#include <string>

namespace midday::expr {

// CLI exit code for every compile-time expression diagnostic (spec section 9
// "3 validation"; cli::Exit::Validation).
inline constexpr int kCompileDiagExitCode = 3;

struct Diag {
    std::string code;    // "expr.parse", "expr.type", ... (list above)
    std::string message; // one-line human summary, no location prefix
    std::string origin;  // file path or source label, "<expr>" when unnamed
    int line = 1;
    int col = 1;
    std::size_t offset = 0; // byte offset of the offending position

    [[nodiscard]] std::string to_string() const; // "origin:line:col: message"

    // Lift into the engine Error envelope: code as-is, message prefixed with
    // the location, details {file, line, col, offset} — the same shape as
    // base::to_error(JsonParseError).
    [[nodiscard]] base::Error to_error() const;
};

} // namespace midday::expr
