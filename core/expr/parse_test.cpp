// expr.parse.* / expr.reject.* — syntax, and the REJECTED-AT-PARSE contract:
// side-effecting or stateful syntax (assignment, compound assignment,
// increment, statements, loops) never produces a program; each rejection is
// a structured diagnostic with origin:line:col, validation-class (exit 3).

#include "core/base/error.h"
#include "core/expr/expr.h"
#include "doctest/doctest.h"
#include "testkit/doctest_unwrap.h"

#include <ostream> // MSVC: doctest stringifies string_view via operator<<
#include <string>
#include <string_view>

using namespace midday;

namespace {

expr::Diag reject(std::string_view source) {
    const expr::EnvSpec env;
    expr::CompileResult result = expr::compile(source, env, "test.yaml");
    REQUIRE(!result);
    return testkit::unwrap(result.diag);
}

expr::Diag reject_with(std::string_view source, const expr::EnvSpec& env) {
    expr::CompileResult result = expr::compile(source, env, "test.yaml");
    REQUIRE(!result);
    return testkit::unwrap(result.diag);
}

} // namespace

TEST_CASE("expr.reject: assignment '=' fails at parse with a structured diagnostic") {
    const expr::Diag diag = reject("x = 1");
    CHECK(diag.code == "expr.side_effect");
    CHECK(diag.message.find("assignment '='") != std::string::npos);
    CHECK(diag.message.find("side-effect-free") != std::string::npos);
    CHECK(diag.origin == "test.yaml");
    CHECK(diag.line == 1);
    CHECK(diag.col == 3);
    CHECK(diag.offset == 2);
}

TEST_CASE("expr.reject: compound assignment '+=' fails at parse") {
    const expr::Diag diag = reject("x += 1");
    CHECK(diag.code == "expr.side_effect");
    CHECK(diag.message.find("'+='") != std::string::npos);
}

TEST_CASE("expr.reject: every compound assignment and mutation form") {
    for (const std::string_view source : {"x -= 1", "x *= 2", "x /= 2", "x %= 2"}) {
        const expr::Diag diag = reject(source);
        CHECK(diag.code == "expr.side_effect");
    }
}

TEST_CASE("expr.reject: increment '++' fails at parse") {
    const expr::Diag diag = reject("x++");
    CHECK(diag.code == "expr.side_effect");
    CHECK(diag.message.find("'++'") != std::string::npos);
    CHECK(reject("--x").code == "expr.side_effect");
}

TEST_CASE("expr.reject: statement separator ';' fails at parse") {
    const expr::Diag diag = reject("1; 2");
    CHECK(diag.code == "expr.side_effect");
    CHECK(diag.message.find("';'") != std::string::npos);
    CHECK(diag.message.find("no statements") != std::string::npos);
}

TEST_CASE("expr.reject: statement/loop keywords are reserved with guidance") {
    CHECK(reject("if x > 1 then 2").code == "expr.side_effect");
    CHECK(reject("while true").code == "expr.side_effect");
    CHECK(reject("for i").code == "expr.side_effect");
    CHECK(reject("let x").code == "expr.side_effect");
    CHECK(reject("return 1").code == "expr.side_effect");
    CHECK(reject("if x").message.find("cond ? a : b") != std::string::npos);
}

TEST_CASE("expr.reject: diagnostics are validation-class errors (CLI exit 3)") {
    // The Error-level assertion of the exit-3 mapping: expr.* compile
    // diagnostics carry the validation exit class (spec section 9), pinned
    // before the CLI verbs consume them.
    CHECK(expr::kCompileDiagExitCode == 3);
    const base::Error error = reject("x = 1").to_error();
    CHECK(error.code == "expr.side_effect");
    CHECK(error.message == "test.yaml:1:3: assignment '=' is not allowed (use '==' to compare)"
                           " — expressions are side-effect-free by construction");
    REQUIRE(error.details.find("file") != nullptr);
    CHECK(error.details.find("file")->as_string() == "test.yaml");
    CHECK(error.details.find("line")->as_int() == 1);
    CHECK(error.details.find("col")->as_int() == 3);
    CHECK(error.details.find("offset")->as_int() == 2);
}

TEST_CASE("expr.parse: location tracking spans lines (YAML multi-line strings)") {
    const expr::Diag diag = reject("1 +\n  = 2");
    CHECK(diag.code == "expr.side_effect");
    CHECK(diag.line == 2);
    CHECK(diag.col == 3);
}

TEST_CASE("expr.parse: syntax errors are structured, not exceptions") {
    CHECK(reject("1 +").code == "expr.parse");
    CHECK(reject("(1").code == "expr.parse");
    CHECK(reject("1 2").code == "expr.parse");
    CHECK(reject("min(1,").code == "expr.parse");
    CHECK(reject("a & b").code == "expr.parse");
    CHECK(reject("a | b").code == "expr.parse");
    CHECK(reject("@").code == "expr.parse");
    CHECK(reject("'oops").code == "expr.parse");                // unterminated string
    CHECK(reject("'bad\\q'").code == "expr.parse");             // unknown escape
    CHECK(reject("007").code == "expr.parse");                  // leading zeros
    CHECK(reject("1e").code == "expr.parse");                   // empty exponent
    CHECK(reject("99999999999999999999").code == "expr.parse"); // > int64
}

TEST_CASE("expr.parse: nesting depth is capped with a structured diagnostic") {
    std::string deep;
    for (int i = 0; i < 200; ++i)
        deep += '(';
    deep += '1';
    for (int i = 0; i < 200; ++i)
        deep += ')';
    CHECK(reject(deep).code == "expr.too_complex");
}

TEST_CASE("expr.parse: grammar accepts the conventional surface") {
    expr::EnvSpec env;
    env.declare("a", expr::ValueType::kFloat);
    env.declare("b", expr::ValueType::kFloat);
    env.declare("flag", expr::ValueType::kBool);
    for (const std::string_view source : {
             "1 + 2 * 3",
             "-a",
             "(a + b) / 2.0",
             "a < b && b <= 3.5 || not flag",
             "a < b and b <= 3.5 or !flag",
             "flag ? a : b",
             "clamp(a, 0, 1)",
             "min(a, b) > 0.25 ? a * 2 : b / 2",
             "1.5e3 > a",
             "true != false",
         }) {
        expr::CompileResult result = expr::compile(source, env, "ok.yaml");
        CHECK_MESSAGE(static_cast<bool>(result), source);
    }
    // Trailing garbage after a complete expression is refused.
    CHECK(reject_with("a b", env).code == "expr.parse");
}
