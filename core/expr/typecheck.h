// core/expr/typecheck.h — static typechecking, at compile time, against the
// declared variable types (env.h). Annotates the AST in place; type errors
// are structured diagnostics (expr.type / expr.unknown_variable /
// expr.unknown_function / expr.arity), never runtime surprises.
//
// NUMERIC MODEL (minimal by design — README has the full table):
//   * two numeric types: int (int64) and float (float32, the sim scalar).
//   * exactly ONE implicit coercion: int -> float, applied where a float is
//     required (mixed arithmetic/comparison operands, float parameters,
//     unified ?: branches). NEVER float -> int (that is the explicit int()).
//   * one compile-time fold: a string LITERAL becomes a name constant where
//     a name is expected (== / != against a name, name-typed parameters) —
//     interning happens at compile, so eval never allocates. A string
//     VARIABLE does not coerce (that would intern at eval).
//
// Identifier paths resolve to the LONGEST declared variable prefix; at most
// one remaining segment is a component access (x/y/z/w on vec2/vec3/vec4/
// quat, width-checked). Everything else is expr.unknown_variable.

#pragma once

#include "core/expr/ast.h"
#include "core/expr/diag.h"
#include "core/expr/env.h"

#include <optional>
#include <string_view>

namespace midday::expr {

// Annotates `root` (types, slots, lanes, builtin indices, coercions).
// Returns the first diagnostic, or nullopt on success.
std::optional<Diag> typecheck(AstNode& root, const EnvSpec& env, std::string_view origin);

} // namespace midday::expr
