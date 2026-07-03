// expr.type.* — static typechecking at compile time: unknown identifiers,
// arity and argument types, operator typing, the single int->float coercion,
// the string-literal->name fold, and member access — all structured
// diagnostics, never runtime surprises.

#include "core/expr/expr.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"

#include <ostream> // MSVC: doctest stringifies string_view via operator<<
#include <string>
#include <string_view>

using namespace midday;

namespace {

expr::EnvSpec test_env() {
    expr::EnvSpec env;
    env.declare("speed", expr::ValueType::kFloat);
    env.declare("count", expr::ValueType::kInt);
    env.declare("alive", expr::ValueType::kBool);
    env.declare("tag", expr::ValueType::kString);
    env.declare("state", expr::ValueType::kName);
    env.declare("pos", expr::ValueType::kVec3);
    env.declare("uv", expr::ValueType::kVec2);
    env.declare("facing", expr::ValueType::kQuat);
    env.declare("health.current", expr::ValueType::kFloat); // dotted decl
    return env;
}

expr::Diag reject(std::string_view source) {
    expr::CompileResult result = expr::compile(source, test_env(), "test.yaml");
    REQUIRE(!result);
    return testkit::unwrap(result.diag);
}

expr::ValueType type_of(std::string_view source) {
    expr::CompileResult result = expr::compile(source, test_env(), "test.yaml");
    REQUIRE_MESSAGE(static_cast<bool>(result),
                    source << " -> " << (result.diag ? result.diag->message : ""));
    return testkit::unwrap(result.program).result_type();
}

} // namespace

TEST_CASE("expr.type: unknown function is a structured diagnostic") {
    const expr::Diag diag = reject("bogus(1)");
    CHECK(diag.code == "expr.unknown_function");
    CHECK(diag.message.find("bogus") != std::string::npos);
    CHECK(reject("math.min(1, 2)").code == "expr.unknown_function");
}

TEST_CASE("expr.type: unknown variable is a structured diagnostic with a hint") {
    const expr::Diag diag = reject("bogus > 1");
    CHECK(diag.code == "expr.unknown_variable");
    // A dotted declaration is suggested when the path is its prefix.
    const expr::Diag hinted = reject("health > 1");
    CHECK(hinted.code == "expr.unknown_variable");
    CHECK(hinted.message.find("health.current") != std::string::npos);
}

TEST_CASE("expr.type: arity mismatch is a structured diagnostic") {
    const expr::Diag diag = reject("min(1)");
    CHECK(diag.code == "expr.arity");
    CHECK(diag.message.find("expects 2") != std::string::npos);
    CHECK(diag.message.find("got 1") != std::string::npos);
    CHECK(reject("clamp(1, 2, 3, 4)").code == "expr.arity");
}

TEST_CASE("expr.type: argument type mismatch names the parameter") {
    const expr::Diag diag = reject("min(tag, 1)");
    CHECK(diag.code == "expr.type");
    CHECK(diag.message.find("'a'") != std::string::npos);
    CHECK(diag.message.find("string") != std::string::npos);
    CHECK(reject("dot(pos, 1)").code == "expr.type");
    CHECK(reject("length(uv)").code == "expr.type"); // vec3-only inventory
}

TEST_CASE("expr.type: operator typing rejects nonsense") {
    CHECK(reject("1 + tag").code == "expr.type");
    CHECK(reject("pos + uv").code == "expr.type");
    CHECK(reject("pos + speed").code == "expr.type");
    CHECK(reject("alive + 1").code == "expr.type");
    CHECK(reject("pos < pos").code == "expr.type");
    CHECK(reject("speed % 2").code == "expr.type"); // % is integer-only
    CHECK(reject("not speed").code == "expr.type");
    CHECK(reject("-facing").code == "expr.type");
    CHECK(reject("alive && count").code == "expr.type");
    CHECK(reject("speed / pos").code == "expr.type");
    CHECK(reject("alive ? 1 : tag").code == "expr.type");
    CHECK(reject("speed ? 1 : 2").code == "expr.type"); // condition must be bool
    CHECK(reject("pos == uv").code == "expr.type");
}

TEST_CASE("expr.type: the single implicit coercion is int -> float") {
    CHECK(type_of("count + count") == expr::ValueType::kInt);
    CHECK(type_of("count + speed") == expr::ValueType::kFloat);
    CHECK(type_of("count / 2") == expr::ValueType::kInt); // int division
    CHECK(type_of("count / 2.0") == expr::ValueType::kFloat);
    CHECK(type_of("abs(count)") == expr::ValueType::kFloat); // param coercion
    CHECK(type_of("alive ? count : speed") == expr::ValueType::kFloat);
    CHECK(type_of("pos * 2") == expr::ValueType::kVec3); // scalar int coerces
    // Never float -> int implicitly; int() is the explicit conversion.
    CHECK(reject("count % int(speed) == 0 && speed % 2 > 0").code == "expr.type");
    CHECK(type_of("int(speed)") == expr::ValueType::kInt);
}

TEST_CASE("expr.type: names compare against name variables and string literals") {
    CHECK(type_of("state == 'attacking'") == expr::ValueType::kBool);
    CHECK(type_of("'attacking' != state") == expr::ValueType::kBool);
    CHECK(type_of("tag == 'boss'") == expr::ValueType::kBool); // string == string
    // A string VARIABLE cannot fold (would intern at eval time).
    const expr::Diag diag = reject("state == tag");
    CHECK(diag.code == "expr.type");
    CHECK(diag.message.find("LITERAL") != std::string::npos);
}

TEST_CASE("expr.type: member access is width-checked component access") {
    CHECK(type_of("pos.x + pos.y + pos.z") == expr::ValueType::kFloat);
    CHECK(type_of("uv.y") == expr::ValueType::kFloat);
    CHECK(type_of("facing.w") == expr::ValueType::kFloat);
    CHECK(type_of("normalize(pos).x") == expr::ValueType::kFloat);   // on call result
    CHECK(type_of("health.current < 30") == expr::ValueType::kBool); // dotted var
    CHECK(reject("uv.z").code == "expr.type");
    CHECK(reject("pos.w").code == "expr.type");
    CHECK(reject("speed.x").code == "expr.type");
    CHECK(reject("pos.length").code == "expr.type");
    CHECK(reject("pos.x.y").code == "expr.type");
}

TEST_CASE("expr.type: quat surface — compose, rotate, compare, components") {
    CHECK(type_of("facing * facing") == expr::ValueType::kQuat);
    CHECK(type_of("rotate(facing, pos)") == expr::ValueType::kVec3);
    CHECK(type_of("facing == quat(0.0, 0.0, 0.0, 1.0)") == expr::ValueType::kBool);
    CHECK(reject("facing + facing").code == "expr.type");
    CHECK(reject("facing * 2.0").code == "expr.type");
}
