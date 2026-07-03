// core/expr/env.h — the variable-binding environment, split compile/eval:
//
//   * EnvSpec (compile time): ordered variable DECLARATIONS. Declaration
//     index = slot index. Dotted names are first-class ("health.current",
//     "hull.bbox.max") — the binder decides the vocabulary; the language
//     resolves an identifier path against the LONGEST declared prefix and
//     treats remaining segments as component access (vec/quat lanes).
//   * At eval: a flat std::span<const Value> in slot order. No lookups, no
//     hashing, no allocation — binding a variable is writing one array cell.
//
// The caller owns the Value span and must bind each slot with EXACTLY the
// declared type (typechecking made every access type-correct against the
// declarations; a mismatched binding at eval is a host programming error).
// String values must outlive the eval call.

#pragma once

#include "core/expr/value.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace midday::expr {

class EnvSpec {
public:
    struct Var {
        std::string name; // dotted names allowed; exact spelling is the API
        ValueType type;
    };

    // Declares a variable; returns its slot (= declaration index). Duplicate
    // or empty names abort loudly — declaring the environment is host code,
    // like registry registration (D-BUILD-023).
    std::uint16_t declare(std::string_view name, ValueType type);

    [[nodiscard]] const std::vector<Var>& vars() const { return vars_; }

    // Slot of an exact name, or -1. Linear scan: environments are small and
    // this runs at compile time only.
    [[nodiscard]] int find(std::string_view name) const;

private:
    std::vector<Var> vars_;
};

} // namespace midday::expr
