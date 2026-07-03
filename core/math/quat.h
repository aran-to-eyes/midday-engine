// core/math/quat.h — rotation quaternions (x, y, z, w), float32.
//
// NORMALIZE POLICY (explicit, contractual):
//   * Rotation operations — rotate(), to_mat3(), slerp(), nlerp(), inverse() —
//     REQUIRE unit-length input; they never renormalize silently (hidden
//     normalization would hide cost and change bits behind the caller's back).
//   * Constructors that promise a rotation (identity, from_axis_angle,
//     from_mat3 of an orthonormal basis) produce unit quaternions to within
//     one rounding step.
//   * Products of unit quaternions drift by accumulated rounding only;
//     long chains renormalize explicitly via normalized() at points the
//     CALLER chooses (deterministic: same chain, same renormalize points,
//     same bits).
//   * is_normalized() is the debug/test guard for the contract.
//
// Determinism classes:
//   BIT-PORTABLE: mul, conjugate/inverse, dot, normalized, nlerp, rotate,
//                 to_mat3, from_mat3 (arithmetic + sqrt only).
//   LIBM-BOUND:   from_axis_angle (sin/cos), slerp (acos/sin) — deterministic
//                 within one build, not bit-portable across platforms
//                 (see core/math/README.md). Sim code that must be
//                 cross-platform bit-exact uses nlerp and mat/quat paths.

#pragma once

#include "core/math/mat.h"
#include "core/math/vec.h"

#include <cmath>

namespace midday::math {

struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f; // default = identity rotation

    [[nodiscard]] static constexpr Quat identity() { return {}; }

    // LIBM-BOUND (sin/cos). `axis` must be unit length.
    static Quat from_axis_angle(Vec3 axis, float radians) {
        const float half = radians * 0.5f;
        const float s = std::sin(half);
        return {axis.x * s, axis.y * s, axis.z * s, std::cos(half)};
    }

    // Hamilton product: (a * b) rotates by b FIRST, then a — matching the
    // column-vector matrix convention (to_mat3(a * b) == to_mat3(a) * to_mat3(b)).
    friend constexpr Quat operator*(Quat a, Quat b) {
        return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
    }

    friend constexpr bool operator==(Quat a, Quat b) {
        return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
    }

    [[nodiscard]] constexpr Quat conjugate() const { return {-x, -y, -z, w}; }

    // Inverse of a UNIT quaternion (the policy above) is its conjugate.
    [[nodiscard]] constexpr Quat inverse() const { return conjugate(); }

    [[nodiscard]] constexpr float length_squared() const { return x * x + y * y + z * z + w * w; }

    [[nodiscard]] float length() const { return std::sqrt(length_squared()); }

    // Zero quaternion normalizes to identity (no NaN escape into sim state).
    [[nodiscard]] Quat normalized() const {
        const float len = length();
        if (len <= 0.0f)
            return identity();
        const float inv = 1.0f / len;
        return {x * inv, y * inv, z * inv, w * inv};
    }

    [[nodiscard]] bool is_normalized(float eps = 1e-4f) const {
        return almost_equal(length_squared(), 1.0f, eps);
    }

    // Rotate a vector by a UNIT quaternion:  v' = v + w*t + q_v x t,
    // t = 2 (q_v x v)  — the standard optimized sandwich product.
    [[nodiscard]] constexpr Vec3 rotate(Vec3 v) const {
        const Vec3 qv{x, y, z};
        const Vec3 t = 2.0f * cross(qv, v);
        return v + w * t + cross(qv, t);
    }

    // Column-major rotation matrix of a UNIT quaternion.
    [[nodiscard]] constexpr Mat3 to_mat3() const {
        const float xx = x * x, yy = y * y, zz = z * z;
        const float xy = x * y, xz = x * z, yz = y * z;
        const float wx = w * x, wy = w * y, wz = w * z;
        return Mat3::from_cols({1.0f - 2.0f * (yy + zz), 2.0f * (xy + wz), 2.0f * (xz - wy)},
                               {2.0f * (xy - wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz + wx)},
                               {2.0f * (xz + wy), 2.0f * (yz - wx), 1.0f - 2.0f * (xx + yy)});
    }

    // Rotation matrix -> quaternion, Shepperd's method: branch on the largest
    // of trace / diagonal elements for numerical robustness (the same shape
    // Godot Basis::get_quaternion uses). `m` must be a pure rotation
    // (orthonormal, det +1). Branches compare computed floats — same input
    // bits take the same branch on every platform: deterministic.
    static Quat from_mat3(const Mat3& m);
};

constexpr float dot(Quat a, Quat b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

// Normalized lerp: not constant angular velocity, but BIT-PORTABLE and the
// right default for sim-side blending. Takes the shortest arc (negates b
// when dot < 0).
inline Quat nlerp(Quat a, Quat b, float t) {
    const float sign = dot(a, b) < 0.0f ? -1.0f : 1.0f;
    const Quat blended{lerp(a.x, sign * b.x, t),
                       lerp(a.y, sign * b.y, t),
                       lerp(a.z, sign * b.z, t),
                       lerp(a.w, sign * b.w, t)};
    return blended.normalized();
}

// Spherical lerp: constant angular velocity. LIBM-BOUND (acos/sin) — falls
// back to nlerp when the arc is tiny (sin(theta) underflow guard).
inline Quat slerp(Quat a, Quat b, float t) {
    float cos_theta = dot(a, b);
    float sign = 1.0f;
    if (cos_theta < 0.0f) { // shortest arc
        cos_theta = -cos_theta;
        sign = -1.0f;
    }
    if (cos_theta > 0.9995f)
        return nlerp(a, {sign * b.x, sign * b.y, sign * b.z, sign * b.w}, t);
    const float theta = std::acos(cos_theta);
    const float inv_sin = 1.0f / std::sin(theta);
    const float wa = std::sin((1.0f - t) * theta) * inv_sin;
    const float wb = std::sin(t * theta) * inv_sin * sign;
    return {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
}

inline Quat Quat::from_mat3(const Mat3& m) {
    const float trace = m.element(0, 0) + m.element(1, 1) + m.element(2, 2);
    if (trace > 0.0f) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f; // s = 4w
        return {(m.element(2, 1) - m.element(1, 2)) / s,
                (m.element(0, 2) - m.element(2, 0)) / s,
                (m.element(1, 0) - m.element(0, 1)) / s,
                0.25f * s};
    }
    if (m.element(0, 0) > m.element(1, 1) && m.element(0, 0) > m.element(2, 2)) {
        const float s =
            std::sqrt(1.0f + m.element(0, 0) - m.element(1, 1) - m.element(2, 2)) * 2.0f; // s = 4x
        return {0.25f * s,
                (m.element(0, 1) + m.element(1, 0)) / s,
                (m.element(0, 2) + m.element(2, 0)) / s,
                (m.element(2, 1) - m.element(1, 2)) / s};
    }
    if (m.element(1, 1) > m.element(2, 2)) {
        const float s =
            std::sqrt(1.0f + m.element(1, 1) - m.element(0, 0) - m.element(2, 2)) * 2.0f; // s = 4y
        return {(m.element(0, 1) + m.element(1, 0)) / s,
                0.25f * s,
                (m.element(1, 2) + m.element(2, 1)) / s,
                (m.element(0, 2) - m.element(2, 0)) / s};
    }
    const float s =
        std::sqrt(1.0f + m.element(2, 2) - m.element(0, 0) - m.element(1, 1)) * 2.0f; // s = 4z
    return {(m.element(0, 2) + m.element(2, 0)) / s,
            (m.element(1, 2) + m.element(2, 1)) / s,
            0.25f * s,
            (m.element(1, 0) - m.element(0, 1)) / s};
}

} // namespace midday::math
