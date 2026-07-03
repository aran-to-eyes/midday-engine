#include "core/expr/functions.h"

#include "core/base/json.h"
#include "core/base/name.h"

#include <cmath>
#include <cstddef>
#include <iterator>
#include <string>
#include <utility>

namespace midday::expr {
namespace {

// Determinism note for every native below (D-BUILD-019, core/math/README.md):
// only IEEE + - * /, sqrt, comparisons, and the EXACT integral roundings
// (floor/ceil/trunc/round on float) appear — all BIT-PORTABLE under the
// pinned FP flags. No libm transcendentals.

Value fn_abs(const Value* a) noexcept {
    return Value::of_float(std::fabs(a[0].u.f));
}

Value fn_sign(const Value* a) noexcept {
    const float x = a[0].u.f;
    if (x > 0.0f)
        return Value::of_float(1.0f);
    if (x < 0.0f)
        return Value::of_float(-1.0f);
    return Value::of_float(0.0f); // ±0 and NaN
}

Value fn_floor(const Value* a) noexcept {
    return Value::of_float(std::floor(a[0].u.f));
}

Value fn_ceil(const Value* a) noexcept {
    return Value::of_float(std::ceil(a[0].u.f));
}

Value fn_round(const Value* a) noexcept {
    return Value::of_float(std::round(a[0].u.f));
}

Value fn_trunc(const Value* a) noexcept {
    return Value::of_float(std::trunc(a[0].u.f));
}

Value fn_fract(const Value* a) noexcept {
    return Value::of_float(a[0].u.f - std::floor(a[0].u.f));
}

Value fn_sqrt(const Value* a) noexcept {
    return Value::of_float(std::sqrt(a[0].u.f));
}

Value fn_min(const Value* a) noexcept {
    return Value::of_float(a[1].u.f < a[0].u.f ? a[1].u.f : a[0].u.f);
}

Value fn_max(const Value* a) noexcept {
    return Value::of_float(a[0].u.f < a[1].u.f ? a[1].u.f : a[0].u.f);
}

Value fn_clamp(const Value* a) noexcept {
    return Value::of_float(math::clamp(a[0].u.f, a[1].u.f, a[2].u.f));
}

Value fn_saturate(const Value* a) noexcept {
    return Value::of_float(math::saturate(a[0].u.f));
}

Value fn_lerp(const Value* a) noexcept {
    return Value::of_float(math::lerp(a[0].u.f, a[1].u.f, a[2].u.f));
}

Value fn_vec2(const Value* a) noexcept {
    return Value::of_vec2({a[0].u.f, a[1].u.f});
}

Value fn_vec3(const Value* a) noexcept {
    return Value::of_vec3({a[0].u.f, a[1].u.f, a[2].u.f});
}

Value fn_vec4(const Value* a) noexcept {
    return Value::of_vec4({a[0].u.f, a[1].u.f, a[2].u.f, a[3].u.f});
}

Value fn_quat(const Value* a) noexcept {
    return Value::of_quat({a[0].u.f, a[1].u.f, a[2].u.f, a[3].u.f});
}

Value fn_dot(const Value* a) noexcept {
    return Value::of_float(math::dot(a[0].as_vec3(), a[1].as_vec3()));
}

Value fn_cross(const Value* a) noexcept {
    return Value::of_vec3(math::cross(a[0].as_vec3(), a[1].as_vec3()));
}

Value fn_length(const Value* a) noexcept {
    return Value::of_float(a[0].as_vec3().length());
}

Value fn_length_squared(const Value* a) noexcept {
    return Value::of_float(a[0].as_vec3().length_squared());
}

Value fn_normalize(const Value* a) noexcept {
    return Value::of_vec3(a[0].as_vec3().normalized());
}

Value fn_distance(const Value* a) noexcept {
    return Value::of_float((a[0].as_vec3() - a[1].as_vec3()).length());
}

Value fn_distance_squared(const Value* a) noexcept {
    return Value::of_float((a[0].as_vec3() - a[1].as_vec3()).length_squared());
}

Value fn_rotate(const Value* a) noexcept {
    return Value::of_vec3(a[0].as_quat().rotate(a[1].as_vec3()));
}

constexpr ValueType kI = ValueType::kInt;
constexpr ValueType kF = ValueType::kFloat;
constexpr ValueType kV2 = ValueType::kVec2;
constexpr ValueType kV3 = ValueType::kVec3;
constexpr ValueType kV4 = ValueType::kVec4;
constexpr ValueType kQ = ValueType::kQuat;

// Canonical inventory order = registration order = engine_api.json order.
// APPEND new functions at the end of their group; never reorder (the API
// dump's byte stability and compat-diff readability depend on it).
constexpr Builtin kBuiltins[] = {
    // ---- conversions (compile to checked opcodes, fn == nullptr) --------
    {.name = "int",
     .doc = "Convert float to int, truncating toward zero. NaN or a value "
            "outside int64 range is a runtime expression error.",
     .param_names = {"x"},
     .fn = nullptr,
     .arity = 1,
     .returns = kI,
     .params = {kF}},
    {.name = "float",
     .doc = "Convert int to float32 (IEEE round-to-nearest).",
     .param_names = {"x"},
     .fn = nullptr,
     .arity = 1,
     .returns = kF,
     .params = {kI}},
    // ---- scalar math (float32, BIT-PORTABLE) ----------------------------
    {.name = "abs",
     .doc = "Absolute value.",
     .param_names = {"x"},
     .fn = fn_abs,
     .arity = 1,
     .returns = kF,
     .params = {kF}},
    {.name = "sign",
     .doc = "1 for positive, -1 for negative, 0 for zero and NaN.",
     .param_names = {"x"},
     .fn = fn_sign,
     .arity = 1,
     .returns = kF,
     .params = {kF}},
    {.name = "floor",
     .doc = "Largest integral value <= x (exact).",
     .param_names = {"x"},
     .fn = fn_floor,
     .arity = 1,
     .returns = kF,
     .params = {kF}},
    {.name = "ceil",
     .doc = "Smallest integral value >= x (exact).",
     .param_names = {"x"},
     .fn = fn_ceil,
     .arity = 1,
     .returns = kF,
     .params = {kF}},
    {.name = "round",
     .doc = "Nearest integral value, halves away from zero (exact).",
     .param_names = {"x"},
     .fn = fn_round,
     .arity = 1,
     .returns = kF,
     .params = {kF}},
    {.name = "trunc",
     .doc = "Integral value toward zero (exact).",
     .param_names = {"x"},
     .fn = fn_trunc,
     .arity = 1,
     .returns = kF,
     .params = {kF}},
    {.name = "fract",
     .doc = "Fractional part: x - floor(x).",
     .param_names = {"x"},
     .fn = fn_fract,
     .arity = 1,
     .returns = kF,
     .params = {kF}},
    {.name = "sqrt",
     .doc = "Square root (IEEE correctly rounded). Negative input yields NaN; "
            "comparisons against NaN are false.",
     .param_names = {"x"},
     .fn = fn_sqrt,
     .arity = 1,
     .returns = kF,
     .params = {kF}},
    {.name = "min",
     .doc = "Smaller of a and b (b < a selects b; NaN operands select a).",
     .param_names = {"a", "b"},
     .fn = fn_min,
     .arity = 2,
     .returns = kF,
     .params = {kF, kF}},
    {.name = "max",
     .doc = "Larger of a and b (a < b selects b; NaN operands select a).",
     .param_names = {"a", "b"},
     .fn = fn_max,
     .arity = 2,
     .returns = kF,
     .params = {kF, kF}},
    {.name = "clamp",
     .doc = "x clamped to [lo, hi].",
     .param_names = {"x", "lo", "hi"},
     .fn = fn_clamp,
     .arity = 3,
     .returns = kF,
     .params = {kF, kF, kF}},
    {.name = "saturate",
     .doc = "x clamped to [0, 1].",
     .param_names = {"x"},
     .fn = fn_saturate,
     .arity = 1,
     .returns = kF,
     .params = {kF}},
    {.name = "lerp",
     .doc = "Linear blend a + (b - a) * t.",
     .param_names = {"a", "b", "t"},
     .fn = fn_lerp,
     .arity = 3,
     .returns = kF,
     .params = {kF, kF, kF}},
    // ---- constructors ----------------------------------------------------
    {.name = "vec2",
     .doc = "Construct a vec2 from components.",
     .param_names = {"x", "y"},
     .fn = fn_vec2,
     .arity = 2,
     .returns = kV2,
     .params = {kF, kF}},
    {.name = "vec3",
     .doc = "Construct a vec3 from components.",
     .param_names = {"x", "y", "z"},
     .fn = fn_vec3,
     .arity = 3,
     .returns = kV3,
     .params = {kF, kF, kF}},
    {.name = "vec4",
     .doc = "Construct a vec4 from components.",
     .param_names = {"x", "y", "z", "w"},
     .fn = fn_vec4,
     .arity = 4,
     .returns = kV4,
     .params = {kF, kF, kF, kF}},
    {.name = "quat",
     .doc = "Construct a quaternion from raw components (NOT normalized; "
            "rotation use requires unit length — core/math policy).",
     .param_names = {"x", "y", "z", "w"},
     .fn = fn_quat,
     .arity = 4,
     .returns = kQ,
     .params = {kF, kF, kF, kF}},
    // ---- vec3 geometry (THE sim vector; see README for the vec2/vec4 note)
    {.name = "dot",
     .doc = "Dot product.",
     .param_names = {"a", "b"},
     .fn = fn_dot,
     .arity = 2,
     .returns = kF,
     .params = {kV3, kV3}},
    {.name = "cross",
     .doc = "Cross product (right-handed).",
     .param_names = {"a", "b"},
     .fn = fn_cross,
     .arity = 2,
     .returns = kV3,
     .params = {kV3, kV3}},
    {.name = "length",
     .doc = "Euclidean length.",
     .param_names = {"v"},
     .fn = fn_length,
     .arity = 1,
     .returns = kF,
     .params = {kV3}},
    {.name = "length_squared",
     .doc = "Squared length (no sqrt — cheaper for radius checks).",
     .param_names = {"v"},
     .fn = fn_length_squared,
     .arity = 1,
     .returns = kF,
     .params = {kV3}},
    {.name = "normalize",
     .doc = "Unit vector; the zero vector normalizes to zero (core/math policy).",
     .param_names = {"v"},
     .fn = fn_normalize,
     .arity = 1,
     .returns = kV3,
     .params = {kV3}},
    {.name = "distance",
     .doc = "Euclidean distance between points.",
     .param_names = {"a", "b"},
     .fn = fn_distance,
     .arity = 2,
     .returns = kF,
     .params = {kV3, kV3}},
    {.name = "distance_squared",
     .doc = "Squared distance (no sqrt — cheaper for radius checks).",
     .param_names = {"a", "b"},
     .fn = fn_distance_squared,
     .arity = 2,
     .returns = kF,
     .params = {kV3, kV3}},
    {.name = "rotate",
     .doc = "Rotate v by the UNIT quaternion q.",
     .param_names = {"q", "v"},
     .fn = fn_rotate,
     .arity = 2,
     .returns = kV3,
     .params = {kQ, kV3}},
};

} // namespace

std::span<const Builtin> builtins() {
    return kBuiltins;
}

int find_builtin(std::string_view name) {
    for (std::size_t i = 0; i < std::size(kBuiltins); ++i)
        if (kBuiltins[i].name == name)
            return static_cast<int>(i);
    return -1;
}

void register_expr_functions(reflect::Registry& registry) {
    for (const Builtin& builtin : builtins()) {
        reflect::MethodDesc desc;
        desc.name = base::Name(builtin.name);
        for (int i = 0; i < builtin.arity; ++i)
            desc.params.push_back(reflect::ParamDesc{
                base::Name(builtin.param_names[i]), to_type_desc(builtin.params[i]), base::Json()});
        desc.returns = to_type_desc(builtin.returns);
        desc.doc = std::string(builtin.doc);
        registry.add_function(std::move(desc));
    }
}

} // namespace midday::expr
