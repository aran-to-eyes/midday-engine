// core/expr/parser.h — recursive-descent parser for the expression grammar.
//
//   expr        := ternary
//   ternary     := or_expr ('?' expr ':' ternary)?          (right-assoc)
//   or_expr     := and_expr (('or' | '||') and_expr)*
//   and_expr    := equality (('and' | '&&') equality)*
//   equality    := relational (('==' | '!=') relational)*
//   relational  := additive (('<' | '<=' | '>' | '>=') additive)*
//   additive    := multiplicative (('+' | '-') multiplicative)*
//   multiplicative := unary (('*' | '/' | '%') unary)*
//   unary       := ('-' | '!' | 'not') unary | postfix
//   postfix     := primary ('.' ident)*
//   primary     := literal | 'true' | 'false' | '(' expr ')'
//                | path | path '(' args ')'
//   path        := ident ('.' ident)*
//   args        := (expr (',' expr)*)?
//
// Nesting depth is capped (kMaxParseDepth) with a structured diagnostic —
// deterministic refusal instead of a stack overflow, like Json::parse.

#pragma once

#include "core/expr/ast.h"
#include "core/expr/diag.h"

#include <optional>
#include <string_view>

namespace midday::expr {

inline constexpr int kMaxParseDepth = 64;

struct ParseResult {
    AstPtr root; // engaged iff !diag
    std::optional<Diag> diag;
};

// Lexes and parses one complete expression (trailing tokens are an error).
ParseResult parse(std::string_view source, std::string_view origin);

} // namespace midday::expr
