// expr.eval.* — evaluation semantics of the compiled form: arithmetic on
// both numeric types, IEEE float behavior, wrapping int behavior, REAL
// short-circuit (the untaken side cannot fault), checked integer division,
// vector/quaternion operators, and the compile-once/eval-many contract.

#include "core/expr/expr.h"
#include "doctest/doctest.h"
#include "testkit/doctest_unwrap.h"

#include <cstdint>
#include <limits>
#include <ostream> // MSVC: doctest stringifies string_view via operator<<
#include <string_view>
#include <vector>

using namespace midday;

namespace {

struct Fixture {
    expr::EnvSpec env;
    std::vector<expr::Value> vars;

    void bind(std::string_view name, expr::Value value) {
        env.declare(name, value.type);
        vars.push_back(value);
    }

    expr::EvalResult run(std::string_view source) {
        expr::CompileResult result = expr::compile(source, env, "eval.yaml");
        REQUIRE_MESSAGE(static_cast<bool>(result),
                        source << " -> " << (result.diag ? result.diag->message : ""));
        return testkit::unwrap(result.program).eval(vars);
    }

    expr::Value ok(std::string_view source) {
        const expr::EvalResult result = run(source);
        REQUIRE(result.status == expr::EvalStatus::kOk);
        return result.value;
    }
};

Fixture make_fixture() {
    Fixture f;
    f.bind("speed", expr::Value::of_float(3.5f));
    f.bind("count", expr::Value::of_int(7));
    f.bind("alive", expr::Value::of_bool(true));
    f.bind("tag", expr::Value::of_string("boss"));
    f.bind("state", expr::Value::of_name(base::Name("attacking")));
    f.bind("pos", expr::Value::of_vec3({1.5f, -2.25f, 4.0f}));
    f.bind("target", expr::Value::of_vec3({-3.0f, 0.5f, 2.0f}));
    f.bind("uv", expr::Value::of_vec2({0.25f, 0.75f}));
    // Unit by construction: 0.6^2 + 0.8^2 == 1 exactly.
    f.bind("facing", expr::Value::of_quat({0.0f, 0.6f, 0.0f, 0.8f}));
    f.bind("health.current", expr::Value::of_float(12.5f));
    return f;
}

} // namespace

TEST_CASE("expr.eval: numeric semantics — int stays int, float is float32") {
    Fixture f = make_fixture();
    CHECK(f.ok("1 + 2 * 3").u.i == 7);
    CHECK(f.ok("7 / 2").u.i == 3); // int division truncates toward zero
    CHECK(f.ok("-7 / 2").u.i == -3);
    CHECK(f.ok("7 % 3").u.i == 1);
    CHECK(f.ok("-7 % 3").u.i == -1); // trunc semantics, like C++
    CHECK(f.ok("7 / 2.0").u.f == 3.5f);
    CHECK(f.ok("speed * 2").u.f == 7.0f);
    CHECK(f.ok("count + 1").u.i == 8);
    CHECK(f.ok("health.current < 30").u.b);
}

TEST_CASE("expr.eval: int overflow wraps two's-complement, deterministically") {
    Fixture f = make_fixture();
    const std::int64_t max = std::numeric_limits<std::int64_t>::max();
    const std::int64_t min = std::numeric_limits<std::int64_t>::min();
    CHECK(f.ok("9223372036854775807 + 1").u.i == min);
    CHECK(f.ok("-9223372036854775807 - 2").u.i == max);
    CHECK(f.ok("-(-9223372036854775807 - 1)").u.i == min); // -INT64_MIN wraps
    CHECK(f.ok("(-9223372036854775807 - 1) / -1").u.i == min);
    CHECK(f.ok("(-9223372036854775807 - 1) % -1").u.i == 0);
}

TEST_CASE("expr.eval: integer division by zero is a structured status") {
    Fixture f = make_fixture();
    CHECK(f.run("1 / 0").status == expr::EvalStatus::kDivZero);
    CHECK(f.run("1 % 0").status == expr::EvalStatus::kDivZero);
    CHECK(f.run("count / (count - 7)").status == expr::EvalStatus::kDivZero);
    const base::Error error = expr::to_error(expr::EvalStatus::kDivZero);
    CHECK(error.code == "expr.div_zero"); // FAILURE-class (exit 1), not validation
}

TEST_CASE("expr.eval: float division by zero is IEEE, not an error") {
    Fixture f = make_fixture();
    CHECK(f.ok("1.0 / 0.0 > 1.0e30").u.b);                  // +inf
    CHECK(f.ok("(0.0 / 0.0) == (0.0 / 0.0)").u.b == false); // NaN != NaN
    CHECK(f.ok("sqrt(0.0 - 1.0) < 0.0").u.b == false);      // NaN compares false
}

TEST_CASE("expr.eval: int() truncates toward zero and range-checks") {
    Fixture f = make_fixture();
    CHECK(f.ok("int(3.9)").u.i == 3);
    CHECK(f.ok("int(0.0 - 3.9)").u.i == -3);
    CHECK(f.ok("float(3) / 2.0").u.f == 1.5f);
    CHECK(f.run("int(1.0e30)").status == expr::EvalStatus::kIntRange);
    CHECK(f.run("int(0.0 / 0.0)").status == expr::EvalStatus::kIntRange);
    CHECK(expr::to_error(expr::EvalStatus::kIntRange).code == "expr.int_range");
}

TEST_CASE("expr.eval: short-circuit is real — the untaken side cannot fault") {
    Fixture f = make_fixture();
    // The classic guard idiom: the division only runs when the guard holds.
    CHECK(f.ok("count != 7 && 10 / (count - 7) > 1").u.b == false);
    CHECK(f.ok("count == 7 || 10 / (count - 7) > 1").u.b == true);
    CHECK(f.ok("count != 7 ? 10 / (count - 7) : -1").u.i == -1);
    // And when the guard passes, the right side runs.
    CHECK(f.ok("count == 7 && 14 / count == 2").u.b == true);
}

TEST_CASE("expr.eval: logic and comparison chain") {
    Fixture f = make_fixture();
    CHECK(f.ok("alive && speed > 3.0 && count >= 7").u.b);
    CHECK(f.ok("not alive || speed <= 3.5").u.b);
    CHECK(f.ok("true != false").u.b);
    CHECK(f.ok("!alive == false").u.b);
}

TEST_CASE("expr.eval: string and name equality") {
    Fixture f = make_fixture();
    CHECK(f.ok("tag == 'boss'").u.b);
    CHECK(f.ok("tag != \"miniboss\"").u.b);
    CHECK(f.ok("state == 'attacking'").u.b);
    CHECK(f.ok("state != 'idle'").u.b);
    CHECK(f.ok("'esc\\'aped' == 'esc\\'aped'").u.b);
}

TEST_CASE("expr.eval: vector operators and functions") {
    Fixture f = make_fixture();
    CHECK(f.ok("(pos + target).x").u.f == -1.5f);
    CHECK(f.ok("(pos - target).y").u.f == -2.75f);
    CHECK(f.ok("(pos * 2).z").u.f == 8.0f);
    CHECK(f.ok("(2 * pos).z").u.f == 8.0f);       // scalar * vec
    CHECK(f.ok("(pos * target).x").u.f == -4.5f); // Hadamard
    CHECK(f.ok("(pos / 2).z").u.f == 2.0f);
    CHECK(f.ok("(-pos).y").u.f == 2.25f);
    CHECK(f.ok("(uv + uv).y").u.f == 1.5f);
    CHECK(f.ok("pos == pos").u.b);
    CHECK(f.ok("pos != target").u.b);
    CHECK(f.ok("dot(pos, target)").u.f == (1.5f * -3.0f) + (-2.25f * 0.5f) + (4.0f * 2.0f));
    CHECK(f.ok("cross(vec3(1, 0, 0), vec3(0, 1, 0)) == vec3(0, 0, 1)").u.b);
    CHECK(f.ok("length(vec3(3, 4, 0))").u.f == 5.0f);
    CHECK(f.ok("length_squared(pos) == dot(pos, pos)").u.b);
    CHECK(f.ok("normalize(vec3(0, 0, 0)) == vec3(0, 0, 0)").u.b); // zero policy
    CHECK(f.ok("distance(pos, pos)").u.f == 0.0f);
    CHECK(f.ok("distance_squared(pos, target) == length_squared(pos - target)").u.b);
}

TEST_CASE("expr.eval: quaternion compose and rotate") {
    Fixture f = make_fixture();
    // Exact arithmetic only (all components integral): 180 degrees about Y.
    CHECK(f.ok("rotate(quat(0, 1, 0, 0), vec3(1, 2, 3)) == vec3(-1, 2, -3)").u.b);
    // Hamilton product: q * q for the 180-degree quat is (0, 0, 0, -1).
    CHECK(f.ok("quat(0, 1, 0, 0) * quat(0, 1, 0, 0) == quat(0, 0, 0, -1)").u.b);
    CHECK(f.ok("facing == quat(0.0, 0.6, 0.0, 0.8)").u.b);
    CHECK(f.ok("facing.w").u.f == 0.8f);
}

TEST_CASE("expr.eval: scalar function inventory behaves") {
    Fixture f = make_fixture();
    CHECK(f.ok("abs(0.0 - 2.5)").u.f == 2.5f);
    CHECK(f.ok("sign(0.0 - 3.0)").u.f == -1.0f);
    CHECK(f.ok("sign(0.0)").u.f == 0.0f);
    CHECK(f.ok("floor(2.7)").u.f == 2.0f);
    CHECK(f.ok("ceil(2.1)").u.f == 3.0f);
    CHECK(f.ok("round(2.5)").u.f == 3.0f); // halves away from zero
    CHECK(f.ok("round(0.0 - 2.5)").u.f == -3.0f);
    CHECK(f.ok("trunc(0.0 - 2.7)").u.f == -2.0f);
    CHECK(f.ok("fract(2.75)").u.f == 0.75f);
    CHECK(f.ok("sqrt(9.0)").u.f == 3.0f);
    CHECK(f.ok("min(2.0, 3.0)").u.f == 2.0f);
    CHECK(f.ok("max(2.0, 3.0)").u.f == 3.0f);
    CHECK(f.ok("clamp(5.0, 0.0, 1.0)").u.f == 1.0f);
    CHECK(f.ok("saturate(0.0 - 1.0)").u.f == 0.0f);
    CHECK(f.ok("lerp(2.0, 4.0, 0.25)").u.f == 2.5f);
}

TEST_CASE("expr.eval: compile once, evaluate many — rebinding is an array write") {
    expr::EnvSpec env;
    env.declare("hp", expr::ValueType::kFloat);
    expr::CompileResult compiled = expr::compile("hp < 30", env, "eval.yaml");
    REQUIRE(static_cast<bool>(compiled));
    const expr::Program& program = testkit::unwrap(compiled.program);
    CHECK(program.var_count() == 1);
    expr::Value vars[1] = {expr::Value::of_float(50.0f)};
    CHECK(program.eval(vars).value.u.b == false);
    vars[0] = expr::Value::of_float(12.0f);
    CHECK(program.eval(vars).value.u.b == true);
    // The compiled form is compact and bounded (cost-model contract).
    CHECK(program.instruction_count() == 3);
    CHECK(program.max_stack() == 2);
    CHECK(program.max_stack() <= expr::kMaxEvalStack);
}
