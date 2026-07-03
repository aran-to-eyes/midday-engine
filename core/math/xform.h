// core/math/xform.h — TRS transform: translation, rotation (unit quat), scale.
//
// Composition order (fixed by the column-vector convention in mat.h):
//   to_mat4() == Mat4::translation(t) * rotation-as-mat4 * Mat4::scaling(s)
// i.e. a point is scaled first, then rotated, then translated.
//
// TRS values do NOT compose closed-form under non-uniform scale (the product
// of two TRS matrices is generally not a TRS matrix). Hierarchy composition
// therefore happens in MATRIX space (m0-scene-hierarchy composes local ->
// world as Mat4 products); from_mat4() is the deterministic projection back
// when a consumer needs TRS components.
//
// Decompose policy (deterministic, documented, tested):
//   * scale.{x,y,z} = length of linear column {0,1,2}.
//   * A negative determinant (mirror) negates scale.x — ONE fixed convention,
//     so decompose is a pure function of the matrix bits.
//   * rotation = Quat::from_mat3 of the scale-normalized columns.
//   * Degenerate (zero-length) columns yield scale 0 and identity rotation
//     for the affected axes rather than NaN.
//
// Determinism class: BIT-PORTABLE (arithmetic + sqrt only).

#pragma once

#include "core/math/mat.h"
#include "core/math/quat.h"
#include "core/math/vec.h"

namespace midday::math {

struct Transform {
    Vec3 translation{};
    Quat rotation{}; // unit (quat.h normalize policy applies)
    Vec3 scale{1, 1, 1};

    [[nodiscard]] static constexpr Transform identity() { return {}; }

    // T * R * S as a single affine Mat4 (columns = rotated, scaled basis).
    [[nodiscard]] Mat4 to_mat4() const;

    // Deterministic TRS decomposition per the policy above.
    static Transform from_mat4(const Mat4& m);

    friend bool operator==(const Transform& a, const Transform& b) {
        return a.translation == b.translation && a.rotation == b.rotation && a.scale == b.scale;
    }
};

} // namespace midday::math
