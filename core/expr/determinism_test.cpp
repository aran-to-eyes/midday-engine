// expr.determinism.* — the determinism exit test: a fixture corpus is
// compiled and evaluated twice (fresh compiles), and again with a shuffled
// compile order; results must be BIT-identical (floats compared by bits,
// never by epsilon). Known-answer bit pins keep the claim falsifiable
// across platforms (D-BUILD-019 ethos: a lane mismatch is a build-config
// bug, not "FP noise").

#include "core/base/name.h"
#include "core/expr/expr.h"
#include "doctest/doctest.h"
#include "testkit/doctest_unwrap.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <ostream> // MSVC: doctest stringifies string_view via operator<<
#include <string_view>
#include <vector>

using namespace midday;

namespace {

// Env values chosen to be exactly representable / unit-by-construction so
// the pinned bits below hold on every platform.
expr::EnvSpec corpus_env() {
    expr::EnvSpec env;
    env.declare("speed", expr::ValueType::kFloat);
    env.declare("count", expr::ValueType::kInt);
    env.declare("alive", expr::ValueType::kBool);
    env.declare("state", expr::ValueType::kName);
    env.declare("tag", expr::ValueType::kString);
    env.declare("pos", expr::ValueType::kVec3);
    env.declare("target", expr::ValueType::kVec3);
    env.declare("facing", expr::ValueType::kQuat);
    env.declare("health.current", expr::ValueType::kFloat);
    return env;
}

std::vector<expr::Value> corpus_vars() {
    return {
        expr::Value::of_float(3.5f),
        expr::Value::of_int(7),
        expr::Value::of_bool(true),
        expr::Value::of_name(base::Name("attacking")),
        expr::Value::of_string("boss"),
        expr::Value::of_vec3({1.5f, -2.25f, 4.0f}),
        expr::Value::of_vec3({-3.0f, 0.5f, 2.0f}),
        expr::Value::of_quat({0.0f, 0.6f, 0.0f, 0.8f}), // unit: .36 + .64 == 1
        expr::Value::of_float(12.5f),
    };
}

constexpr std::string_view kCorpus[] = {
    "speed * 2.0 + health.current / 4.0",
    "count * count - count / 2",
    "alive && health.current < 30.0 || state == 'berserk'",
    "state == 'attacking' && tag == 'boss'",
    "distance(pos, target) < 6.0 ? lerp(0.0, 1.0, saturate(speed / 10.0)) : -1.0",
    "length(cross(normalize(pos), normalize(target)))",
    "dot(pos - target, pos - target) == distance_squared(pos, target)",
    "rotate(facing, pos).x + rotate(facing, pos).z",
    "(facing * facing).y",
    "clamp(sqrt(length_squared(pos)) - 1.0, 0.0, 10.0)",
    "floor(speed) + ceil(speed) + round(speed) + trunc(speed) + fract(speed)",
    "min(max(speed, 1.0), abs(0.0 - 7.5)) * sign(0.0 - 2.0)",
    "int(speed * 100.0) % 16",
    "count != 0 && 1000 / count > 100",
    "(pos * 2 + target * speed).y",
    "float(count) / 3.0",
    "not alive ? 0.0 : saturate(health.current / 100.0)",
    "vec3(1, 2, 3) == vec3(1.0, 2.0, 3.0)",
};

// One digest over every result's bit pattern, in corpus order.
std::uint64_t run_corpus(bool reversed) {
    const expr::EnvSpec env = corpus_env();
    const std::vector<expr::Value> vars = corpus_vars();

    // Compile order must not matter (no global state anywhere in the
    // pipeline): compile forward or reversed, evaluate in corpus order.
    const std::size_t n = std::size(kCorpus);
    std::vector<expr::Program> programs;
    programs.reserve(n);
    std::vector<std::size_t> order(n);
    for (std::size_t i = 0; i < n; ++i)
        order[i] = reversed ? n - 1 - i : i;
    std::vector<int> where(n);
    for (std::size_t at = 0; at < n; ++at) {
        expr::CompileResult result = expr::compile(kCorpus[order[at]], env, "corpus.yaml");
        REQUIRE_MESSAGE(static_cast<bool>(result),
                        kCorpus[order[at]] << " -> " << (result.diag ? result.diag->message : ""));
        where[order[at]] = static_cast<int>(programs.size());
        programs.push_back(std::move(testkit::unwrap(result.program)));
    }

    XXH3_state_t state;
    XXH3_64bits_reset(&state);
    for (std::size_t i = 0; i < n; ++i) {
        const expr::EvalResult result = programs[static_cast<std::size_t>(where[i])].eval(vars);
        REQUIRE(result.status == expr::EvalStatus::kOk);
        const expr::Value& value = result.value;
        switch (value.type) {
        case expr::ValueType::kBool: {
            const std::uint8_t bit = value.u.b ? 1 : 0;
            XXH3_64bits_update(&state, &bit, sizeof bit);
            break;
        }
        case expr::ValueType::kInt:
            XXH3_64bits_update(&state, &value.u.i, sizeof value.u.i);
            break;
        case expr::ValueType::kFloat: {
            const auto bits = std::bit_cast<std::uint32_t>(value.u.f);
            XXH3_64bits_update(&state, &bits, sizeof bits);
            break;
        }
        default:
            XXH3_64bits_update(&state, value.u.lanes, sizeof value.u.lanes);
            break;
        }
    }
    return XXH3_64bits_digest(&state);
}

} // namespace

TEST_CASE("expr.determinism: corpus evaluates bit-identically across independent runs") {
    const std::uint64_t first = run_corpus(/*reversed=*/false);
    const std::uint64_t second = run_corpus(/*reversed=*/false);
    CHECK(first == second);
}

TEST_CASE("expr.determinism: compile order cannot leak into results") {
    CHECK(run_corpus(/*reversed=*/false) == run_corpus(/*reversed=*/true));
}

TEST_CASE("expr.determinism: known-answer bit pins (BIT-PORTABLE claim)") {
    const expr::EnvSpec env = corpus_env();
    const std::vector<expr::Value> vars = corpus_vars();
    const auto bits_of = [&](std::string_view source) {
        expr::CompileResult result = expr::compile(source, env, "pin.yaml");
        REQUIRE(static_cast<bool>(result));
        const expr::EvalResult eval = testkit::unwrap(result.program).eval(vars);
        REQUIRE(eval.status == expr::EvalStatus::kOk);
        REQUIRE(eval.value.type == expr::ValueType::kFloat);
        return std::bit_cast<std::uint32_t>(eval.value.u.f);
    };
    // Exact-arithmetic pins (results are exactly representable):
    CHECK(bits_of("speed * 2.0") == std::bit_cast<std::uint32_t>(7.0f));
    CHECK(bits_of("lerp(2.0, 4.0, 0.25)") == std::bit_cast<std::uint32_t>(2.5f));
    CHECK(bits_of("length(vec3(3, 4, 0))") == std::bit_cast<std::uint32_t>(5.0f));
    // Inexact-arithmetic pins: the bit patterns of correctly-rounded IEEE
    // float32 results — identical on every supported platform under the
    // deterministic-FP flags (core/math/README.md).
    CHECK(bits_of("sqrt(2.0)") == 0x3FB504F3u);
    CHECK(bits_of("1.0 / 3.0") == 0x3EAAAAABu);
    CHECK(bits_of("distance(pos, target)") == 0x40B47CFCu); // sqrt(31.8125f)
}
