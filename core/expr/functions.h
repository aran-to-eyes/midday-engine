// core/expr/functions.h — THE expression-language function inventory.
//
// One table is the single source of truth for three consumers:
//   1. the typechecker/codegen (call resolution, static signatures),
//   2. the evaluator (native function pointers / intrinsic opcodes),
//   3. core/reflect — register_expr_functions() lifts every entry into a
//      MethodDesc free function (D-BUILD-021: method signatures at registry
//      scope, no parallel model), so `midday api dump` (m0-api-json) ships
//      the inventory with param/return types + compat hashes.
//
// Inventory contract:
//   * ONE name = ONE signature = ONE registry entry — no overloads. The
//     language stays 1:1 with what agents read in engine_api.json. If a
//     future node needs an overload set, that is a registry-model decision
//     recorded in DECISIONS.md first (adding entries is cheap; reshaping the
//     mapping is not).
//   * Every function is deterministic, total (never fails at eval), and
//     BIT-PORTABLE (D-BUILD-019): built on IEEE + - * / sqrt and exact
//     integral rounding only — no libm transcendentals anywhere.
//     EXCLUDED and documented (README): sin/cos/tan/asin/acos/atan2, exp,
//     log, pow — LIBM-BOUND. When filters need them, they land as det_*
//     controlled polynomials in core/math first (the det_log recipe).
//   * Vector functions cover vec3, THE sim vector. vec2/vec4/quat still have
//     constructors, arithmetic OPERATORS, component access, and equality —
//     operators are language surface, not registry functions.

#pragma once

#include "core/expr/value.h"
#include "core/reflect/registry.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace midday::expr {

// Native implementation: args are pre-typechecked, length = arity. Total —
// no error channel by design (errors exist only for ops with undefined
// inputs: integer '/' '%' and int(); those compile to checked opcodes).
using NativeFn = Value (*)(const Value* args) noexcept;

inline constexpr int kMaxFunctionArity = 4;

struct Builtin {
    // Field order groups the pointer-sized members first (padding-optimal).
    std::string_view name;
    std::string_view doc;
    std::string_view param_names[kMaxFunctionArity];
    NativeFn fn; // nullptr for the two conversion intrinsics (int, float),
                 // which compile to checked opcodes instead of a call
    std::uint8_t arity;
    ValueType returns;
    ValueType params[kMaxFunctionArity];
};

// The inventory in canonical (registration/API) order.
std::span<const Builtin> builtins();

// Index into builtins() by exact name, or -1.
int find_builtin(std::string_view name);

// Registers every inventory entry as a reflect free function (MethodDesc)
// at the registry's active level. m0-api-json calls this at boot; tests call
// it on isolated registries.
void register_expr_functions(reflect::Registry& registry);

} // namespace midday::expr
