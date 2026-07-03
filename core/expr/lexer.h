// core/expr/lexer.h — tokens for the expression language.
//
// Side-effecting and stateful syntax is rejected HERE, with structured
// diagnostics (expr.side_effect): assignment `=`, compound assignment
// `+= -= *= /= %=`, increment/decrement `++`/`--`, the statement separator
// `;`, and the reserved statement keywords (if, else, while, for, let, var,
// fn, function, return). The language is one expression — no statements, no
// loops, no user recursion, BY CONSTRUCTION.
//
// Literal rules (strict — agent formats are exact, D-BUILD-012 ethos):
//   * int:   decimal digits, no leading zero (except "0"), must fit int64.
//   * float: digits '.' digits, optional exponent — parsed by vendored
//     fast_float (D-BUILD-015): correctly rounded float32, locale-free,
//     byte-deterministic on every platform. Overflow to infinity is an error.
//   * string: '...' or "..." with escapes \\ \' \" \n \t; raw newlines and
//     other control bytes are rejected. Single quotes exist because
//     expressions live inside YAML strings that are usually double-quoted.

#pragma once

#include "core/expr/diag.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace midday::expr {

enum class TokenKind : std::uint8_t {
    kEnd,
    kIntLit,
    kFloatLit,
    kStringLit,
    kIdent,
    kTrue,
    kFalse,
    kAnd, // `and` / `&&`
    kOr,  // `or`  / `||`
    kNot, // `not` / `!`
    kLParen,
    kRParen,
    kComma,
    kDot,
    kPlus,
    kMinus,
    kStar,
    kSlash,
    kPercent,
    kLt,
    kLe,
    kGt,
    kGe,
    kEqEq,
    kNe,
    kQuestion,
    kColon,
};

struct Token {
    TokenKind kind = TokenKind::kEnd;
    std::string_view text; // slice of the source
    int line = 1;
    int col = 1;
    std::size_t offset = 0;
    std::int64_t int_value = 0; // kIntLit
    float float_value = 0.0f;   // kFloatLit
    std::string string_value;   // kStringLit, escapes decoded
};

// Tokenizes `source` into `out` (always terminated by a kEnd token on
// success). Returns the first diagnostic on failure.
std::optional<Diag> lex(std::string_view source, std::string_view origin, std::vector<Token>& out);

} // namespace midday::expr
