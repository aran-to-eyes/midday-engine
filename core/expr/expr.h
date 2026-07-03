// core/expr/expr.h — public facade of THE expression language (m0-expr-lang):
// one deterministic, side-effect-free, statically typed language shared by
// transition `if:` filters, `when:` watchers, and model `params` expressions
// (spec section 4.2 — "no per-context mini-DSLs").
//
// Usage (compile once, evaluate per event — the statechart hot path):
//
//   expr::EnvSpec env;
//   const auto health = env.declare("health.current", expr::ValueType::kFloat);
//   auto compiled = expr::compile("health.current < 30", env, "player.scene.yaml");
//   if (!compiled) { /* compiled.diag: structured, validation-class */ }
//
//   expr::Value vars[1];
//   vars[health] = expr::Value::of_float(12.5f);
//   const expr::EvalResult r = compiled.program->eval(vars);  // noexcept, no alloc
//
// Language reference: core/expr/README.md (grammar, numeric model, coercion
// rules, cost model, function inventory, documented exclusions).

#pragma once

#include "core/expr/diag.h"
#include "core/expr/env.h"
#include "core/expr/functions.h"
#include "core/expr/program.h"
#include "core/expr/value.h"

#include <optional>
#include <string_view>

namespace midday::expr {

struct CompileResult {
    std::optional<Program> program; // engaged iff !diag
    std::optional<Diag> diag;

    explicit operator bool() const { return program.has_value(); }
};

// Parse + typecheck + lower `source` against the declared environment.
// Errors are structured diagnostics (diag.h) — never exceptions, and
// side-effecting syntax never survives past the parse.
CompileResult
compile(std::string_view source, const EnvSpec& env, std::string_view origin = "<expr>");

} // namespace midday::expr
