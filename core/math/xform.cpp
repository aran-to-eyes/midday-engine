#include "core/math/xform.h"

namespace midday::math {

Mat4 Transform::to_mat4() const {
    // T * R * S without materializing intermediates: columns of R scaled by s,
    // translation in column 3. Identical arithmetic to the naive product's
    // effective operations, fewer redundant zero terms.
    const Mat3 r = rotation.to_mat3();
    return Mat4::from_mat3(
        Mat3::from_cols(r.cols[0] * scale.x, r.cols[1] * scale.y, r.cols[2] * scale.z),
        translation);
}

Transform Transform::from_mat4(const Mat4& m) {
    Transform out;
    out.translation = m.cols[3].xyz();

    Vec3 c0 = m.cols[0].xyz();
    Vec3 c1 = m.cols[1].xyz();
    Vec3 c2 = m.cols[2].xyz();
    out.scale = {c0.length(), c1.length(), c2.length()};

    // Mirror convention: a negative determinant folds into scale.x — one
    // fixed choice so identical matrices decompose identically everywhere.
    if (Mat3::from_cols(c0, c1, c2).determinant() < 0.0f)
        out.scale.x = -out.scale.x;

    // Normalize columns into a pure rotation; degenerate axes fall back to
    // the canonical basis vector (scale already records the 0).
    c0 = out.scale.x != 0.0f ? c0 / out.scale.x : Vec3{1, 0, 0};
    c1 = out.scale.y != 0.0f ? c1 / out.scale.y : Vec3{0, 1, 0};
    c2 = out.scale.z != 0.0f ? c2 / out.scale.z : Vec3{0, 0, 1};
    out.rotation = Quat::from_mat3(Mat3::from_cols(c0, c1, c2));
    return out;
}

} // namespace midday::math
