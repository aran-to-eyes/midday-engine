// core/expr/value.h — the expression language's value model (m0-expr-lang).
//
// One deterministic, side-effect-free expression language is shared by
// transition `if:` filters, `when:` watchers, and model `params` expressions
// (spec section 4.2 authoring conventions). Its values are exactly the
// reflect TypeDesc kinds that make sense inside an expression:
//
//   bool · int (int64) · float (float32, THE sim scalar) · string · name ·
//   vec2 · vec3 · vec4 · quat
//
// Documented exclusions (extension is ADDITIVE, never a reshape):
//   * color      — arrives with its first consumer (a color is not filter
//                  logic today; vec4 covers raw component math).
//   * entity_ref — opaque handle; is-in-state and entity predicates arrive as
//                  FUNCTIONS with the statechart node, not as a value kind.
//   * asset_ref  — asset identity is loader domain, meaningless in filters.
//   * array/map  — no iteration in the language (no loops by construction),
//                  so containers have no operations to offer.
//
// Value is a 24-byte tagged union, trivially copyable — the eval stack is a
// flat array of these, no heap anywhere on the eval path. Strings are VIEWS
// (pointer + length) into storage the Program (literals) or the caller
// (bound variables) keeps alive; the language has no string constructors or
// concatenation, precisely so eval can never allocate.

#pragma once

#include "core/base/name.h"
#include "core/math/quat.h"
#include "core/math/vec.h"
#include "core/reflect/type_model.h"

#include <cstdint>
#include <string_view>

namespace midday::expr {

enum class ValueType : std::uint8_t {
    kBool,
    kInt,    // int64, two's-complement wrap on overflow (documented, README)
    kFloat,  // float32 under the deterministic-FP flags — BIT-PORTABLE ops
    kString, // UTF-8 view; comparisons only, no construction
    kName,   // interned base::Name, compared by content-hash id
    kVec2,
    kVec3,
    kVec4,
    kQuat,
};

inline constexpr std::size_t kValueTypeCount = 9;

// Canonical spelling — matches reflect::TypeDesc::canonical() verbatim.
std::string_view to_string(ValueType type);

// The reflect counterpart: expression signatures registered as MethodDesc
// free functions spell their types through this mapping (one vocabulary,
// spec section 8 — schemas generate from reflection).
reflect::TypeDesc to_type_desc(ValueType type);

// Lanes of the float-vector kinds (vec2 -> 2, vec3 -> 3, vec4/quat -> 4).
// Pre: type is a vector kind.
int lane_count(ValueType type);

struct Value {
    ValueType type = ValueType::kBool;

    union Payload {
        bool b;
        std::int64_t i;
        float f;
        float lanes[4]; // vec2/vec3/vec4 (tail zeroed) and quat (x,y,z,w)
        std::uint64_t name_id;

        struct Str {
            const char* data;
            std::uint64_t size;
        } s;

        constexpr Payload() : i(0) {}
    } u;

    static Value of_bool(bool b) {
        Value v;
        v.type = ValueType::kBool;
        v.u.b = b;
        return v;
    }

    static Value of_int(std::int64_t i) {
        Value v;
        v.type = ValueType::kInt;
        v.u.i = i;
        return v;
    }

    static Value of_float(float f) {
        Value v;
        v.type = ValueType::kFloat;
        v.u.f = f;
        return v;
    }

    // `text` must outlive every eval that can see this value (Program owns
    // literal storage; env binders own variable storage).
    static Value of_string(std::string_view text) {
        Value v;
        v.type = ValueType::kString;
        v.u.s = {text.data(), text.size()};
        return v;
    }

    static Value of_name(base::Name name) {
        Value v;
        v.type = ValueType::kName;
        v.u.name_id = name.id();
        return v;
    }

    static Value of_vec2(math::Vec2 vec) {
        Value v;
        v.type = ValueType::kVec2;
        v.u.lanes[0] = vec.x;
        v.u.lanes[1] = vec.y;
        v.u.lanes[2] = 0.0f;
        v.u.lanes[3] = 0.0f;
        return v;
    }

    static Value of_vec3(math::Vec3 vec) {
        Value v;
        v.type = ValueType::kVec3;
        v.u.lanes[0] = vec.x;
        v.u.lanes[1] = vec.y;
        v.u.lanes[2] = vec.z;
        v.u.lanes[3] = 0.0f;
        return v;
    }

    static Value of_vec4(math::Vec4 vec) {
        Value v;
        v.type = ValueType::kVec4;
        v.u.lanes[0] = vec.x;
        v.u.lanes[1] = vec.y;
        v.u.lanes[2] = vec.z;
        v.u.lanes[3] = vec.w;
        return v;
    }

    static Value of_quat(math::Quat quat) {
        Value v;
        v.type = ValueType::kQuat;
        v.u.lanes[0] = quat.x;
        v.u.lanes[1] = quat.y;
        v.u.lanes[2] = quat.z;
        v.u.lanes[3] = quat.w;
        return v;
    }

    [[nodiscard]] std::string_view as_string_view() const {
        return {u.s.data, static_cast<std::size_t>(u.s.size)};
    }

    [[nodiscard]] math::Vec2 as_vec2() const { return {u.lanes[0], u.lanes[1]}; }

    [[nodiscard]] math::Vec3 as_vec3() const { return {u.lanes[0], u.lanes[1], u.lanes[2]}; }

    [[nodiscard]] math::Vec4 as_vec4() const {
        return {u.lanes[0], u.lanes[1], u.lanes[2], u.lanes[3]};
    }

    [[nodiscard]] math::Quat as_quat() const {
        return {u.lanes[0], u.lanes[1], u.lanes[2], u.lanes[3]};
    }
};

} // namespace midday::expr
