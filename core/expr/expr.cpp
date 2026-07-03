#include "core/expr/expr.h"

#include "core/expr/codegen.h"
#include "core/expr/parser.h"
#include "core/expr/typecheck.h"

#include <utility>

namespace midday::expr {

CompileResult compile(std::string_view source, const EnvSpec& env, std::string_view origin) {
    ParseResult parsed = parse(source, origin);
    if (parsed.diag)
        return CompileResult{std::nullopt, std::move(parsed.diag)};
    if (auto diag = typecheck(*parsed.root, env, origin))
        return CompileResult{std::nullopt, std::move(diag)};
    CodegenResult lowered = codegen(*parsed.root, env, origin);
    return CompileResult{std::move(lowered.program), std::move(lowered.diag)};
}

} // namespace midday::expr
