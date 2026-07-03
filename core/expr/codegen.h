// core/expr/codegen.h — lowers the typechecked AST into the compact stack
// program (program.h). Opcodes are selected from the STATIC types the
// checker annotated — eval never dispatches on value tags. `&&`, `||`, and
// `?:` compile to forward jumps (real short-circuit: the untaken side costs
// nothing and cannot fault).

#pragma once

#include "core/expr/ast.h"
#include "core/expr/diag.h"
#include "core/expr/env.h"
#include "core/expr/program.h"

#include <optional>
#include <string_view>

namespace midday::expr {

struct CodegenResult {
    std::optional<Program> program; // engaged iff !diag
    std::optional<Diag> diag;       // only expr.too_complex can occur here
};

CodegenResult codegen(const AstNode& root, const EnvSpec& env, std::string_view origin);

} // namespace midday::expr
