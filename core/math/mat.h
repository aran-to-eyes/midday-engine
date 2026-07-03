// core/math/mat.h — Mat3 / Mat4, float32.
//
// THE column convention for the whole engine (documented once, asserted in
// math.mat tests — every consumer inherits it, never restates it):
//   * Storage is COLUMN-MAJOR: `cols[c]` is column c.
//   * Vectors are COLUMN vectors; a matrix transforms as  v' = M * v.
//   * Composition therefore reads right-to-left: (A * B) * v applies B first.
//   * element(r, c) — row r, column c — is cols[c][r].
//   * For an affine Mat4, the translation lives in column 3 (cols[3].xyz()),
//     and the upper-left 3x3 (columns 0..2) is the linear part.
//   * Coordinate system: right-handed (matches the cross() orientation in vec.h).
//
// Determinism class: BIT-PORTABLE (arithmetic only; no libm calls).
// inverse() uses exact cofactor arithmetic — a singular matrix yields inf/NaN,
// deterministically; callers on sim paths guard with determinant() when the
// input is not known-invertible.

#pragma once

#include "core/math/vec.h"

namespace midday::math {

struct Mat3 {
    // Columns default to identity so `Mat3{}` is the identity matrix.
    Vec3 cols[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};

    [[nodiscard]] static constexpr Mat3 identity() { return {}; }

    static constexpr Mat3 from_cols(Vec3 c0, Vec3 c1, Vec3 c2) { return {{c0, c1, c2}}; }

    static constexpr Mat3 scaling(Vec3 s) { return {{{s.x, 0, 0}, {0, s.y, 0}, {0, 0, s.z}}}; }

    [[nodiscard]] constexpr float element(int row, int col) const { return cols[col][row]; }

    friend constexpr Vec3 operator*(const Mat3& m, Vec3 v) {
        return m.cols[0] * v.x + m.cols[1] * v.y + m.cols[2] * v.z;
    }

    friend constexpr Mat3 operator*(const Mat3& a, const Mat3& b) {
        return {{a * b.cols[0], a * b.cols[1], a * b.cols[2]}};
    }

    friend constexpr bool operator==(const Mat3& a, const Mat3& b) {
        return a.cols[0] == b.cols[0] && a.cols[1] == b.cols[1] && a.cols[2] == b.cols[2];
    }

    [[nodiscard]] constexpr Mat3 transposed() const {
        return {{{cols[0].x, cols[1].x, cols[2].x},
                 {cols[0].y, cols[1].y, cols[2].y},
                 {cols[0].z, cols[1].z, cols[2].z}}};
    }

    [[nodiscard]] constexpr float determinant() const {
        return dot(cols[0], cross(cols[1], cols[2]));
    }

    // Adjugate / determinant. Rows of the inverse are the (scaled) cross
    // products of the input's columns — exact cofactor arithmetic.
    [[nodiscard]] constexpr Mat3 inverse() const {
        const Vec3 r0 = cross(cols[1], cols[2]);
        const Vec3 r1 = cross(cols[2], cols[0]);
        const Vec3 r2 = cross(cols[0], cols[1]);
        const float inv_det = 1.0f / dot(cols[0], r0);
        return Mat3::from_cols({r0.x, r1.x, r2.x}, {r0.y, r1.y, r2.y}, {r0.z, r1.z, r2.z}) *
               inv_det;
    }

    friend constexpr Mat3 operator*(const Mat3& m, float s) {
        return {{m.cols[0] * s, m.cols[1] * s, m.cols[2] * s}};
    }
};

struct Mat4 {
    Vec4 cols[4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};

    [[nodiscard]] static constexpr Mat4 identity() { return {}; }

    static constexpr Mat4 from_cols(Vec4 c0, Vec4 c1, Vec4 c2, Vec4 c3) {
        return {{c0, c1, c2, c3}};
    }

    static constexpr Mat4 translation(Vec3 t) {
        return {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {t.x, t.y, t.z, 1}}};
    }

    static constexpr Mat4 scaling(Vec3 s) {
        return {{{s.x, 0, 0, 0}, {0, s.y, 0, 0}, {0, 0, s.z, 0}, {0, 0, 0, 1}}};
    }

    // Affine embed: linear part from `m`, translation in column 3.
    static constexpr Mat4 from_mat3(const Mat3& m, Vec3 t = {}) {
        return {{{m.cols[0].x, m.cols[0].y, m.cols[0].z, 0},
                 {m.cols[1].x, m.cols[1].y, m.cols[1].z, 0},
                 {m.cols[2].x, m.cols[2].y, m.cols[2].z, 0},
                 {t.x, t.y, t.z, 1}}};
    }

    [[nodiscard]] constexpr Mat3 upper3x3() const {
        return Mat3::from_cols(cols[0].xyz(), cols[1].xyz(), cols[2].xyz());
    }

    [[nodiscard]] constexpr float element(int row, int col) const { return cols[col][row]; }

    friend constexpr Vec4 operator*(const Mat4& m, Vec4 v) {
        return m.cols[0] * v.x + m.cols[1] * v.y + m.cols[2] * v.z + m.cols[3] * v.w;
    }

    friend constexpr Mat4 operator*(const Mat4& a, const Mat4& b) {
        return {{a * b.cols[0], a * b.cols[1], a * b.cols[2], a * b.cols[3]}};
    }

    friend constexpr bool operator==(const Mat4& a, const Mat4& b) {
        return a.cols[0] == b.cols[0] && a.cols[1] == b.cols[1] && a.cols[2] == b.cols[2] &&
               a.cols[3] == b.cols[3];
    }

    // Affine point transform: w = 1, no perspective divide.
    [[nodiscard]] constexpr Vec3 transform_point(Vec3 p) const {
        return (*this * Vec4{p.x, p.y, p.z, 1.0f}).xyz();
    }

    // Direction transform: w = 0 (translation does not apply).
    [[nodiscard]] constexpr Vec3 transform_vector(Vec3 v) const {
        return (*this * Vec4{v.x, v.y, v.z, 0.0f}).xyz();
    }

    [[nodiscard]] constexpr Mat4 transposed() const {
        return {{{cols[0].x, cols[1].x, cols[2].x, cols[3].x},
                 {cols[0].y, cols[1].y, cols[2].y, cols[3].y},
                 {cols[0].z, cols[1].z, cols[2].z, cols[3].z},
                 {cols[0].w, cols[1].w, cols[2].w, cols[3].w}}};
    }

    // Determinant and general inverse via 2x2-subfactor (Laplace) expansion —
    // the standard closed form (see e.g. Godot Projection::invert, GLM
    // inverse). Exact arithmetic, no pivoting: bit-portable.
    [[nodiscard]] constexpr float determinant() const;

    [[nodiscard]] constexpr Mat4 inverse() const;

    // Fast path when the matrix is known affine (bottom row 0 0 0 1):
    // inverse = [ L^-1 | -L^-1 t ].
    [[nodiscard]] constexpr Mat4 inverse_affine() const {
        const Mat3 inv_linear = upper3x3().inverse();
        return from_mat3(inv_linear, -(inv_linear * cols[3].xyz()));
    }
};

namespace detail {

// The six independent 2x2 minors of rows 2..3 (columns of the lower half),
// shared by determinant() and inverse().
struct Mat4Minors {
    float s0, s1, s2, s3, s4, s5; // upper 2x2 minors (rows 0,1)
    float c0, c1, c2, c3, c4, c5; // lower 2x2 minors (rows 2,3)
};

constexpr Mat4Minors mat4_minors(const Mat4& m) {
    return {
        m.element(0, 0) * m.element(1, 1) - m.element(1, 0) * m.element(0, 1),
        m.element(0, 0) * m.element(1, 2) - m.element(1, 0) * m.element(0, 2),
        m.element(0, 0) * m.element(1, 3) - m.element(1, 0) * m.element(0, 3),
        m.element(0, 1) * m.element(1, 2) - m.element(1, 1) * m.element(0, 2),
        m.element(0, 1) * m.element(1, 3) - m.element(1, 1) * m.element(0, 3),
        m.element(0, 2) * m.element(1, 3) - m.element(1, 2) * m.element(0, 3),
        m.element(2, 0) * m.element(3, 1) - m.element(3, 0) * m.element(2, 1),
        m.element(2, 0) * m.element(3, 2) - m.element(3, 0) * m.element(2, 2),
        m.element(2, 0) * m.element(3, 3) - m.element(3, 0) * m.element(2, 3),
        m.element(2, 1) * m.element(3, 2) - m.element(3, 1) * m.element(2, 2),
        m.element(2, 1) * m.element(3, 3) - m.element(3, 1) * m.element(2, 3),
        m.element(2, 2) * m.element(3, 3) - m.element(3, 2) * m.element(2, 3),
    };
}

} // namespace detail

constexpr float Mat4::determinant() const {
    const auto m = detail::mat4_minors(*this);
    return m.s0 * m.c5 - m.s1 * m.c4 + m.s2 * m.c3 + m.s3 * m.c2 - m.s4 * m.c1 + m.s5 * m.c0;
}

constexpr Mat4 Mat4::inverse() const {
    const auto m = detail::mat4_minors(*this);
    const float det =
        m.s0 * m.c5 - m.s1 * m.c4 + m.s2 * m.c3 + m.s3 * m.c2 - m.s4 * m.c1 + m.s5 * m.c0;
    const float id = 1.0f / det;
    Mat4 r;
    r.cols[0] = {(element(1, 1) * m.c5 - element(1, 2) * m.c4 + element(1, 3) * m.c3) * id,
                 (-element(1, 0) * m.c5 + element(1, 2) * m.c2 - element(1, 3) * m.c1) * id,
                 (element(1, 0) * m.c4 - element(1, 1) * m.c2 + element(1, 3) * m.c0) * id,
                 (-element(1, 0) * m.c3 + element(1, 1) * m.c1 - element(1, 2) * m.c0) * id};
    r.cols[1] = {(-element(0, 1) * m.c5 + element(0, 2) * m.c4 - element(0, 3) * m.c3) * id,
                 (element(0, 0) * m.c5 - element(0, 2) * m.c2 + element(0, 3) * m.c1) * id,
                 (-element(0, 0) * m.c4 + element(0, 1) * m.c2 - element(0, 3) * m.c0) * id,
                 (element(0, 0) * m.c3 - element(0, 1) * m.c1 + element(0, 2) * m.c0) * id};
    r.cols[2] = {(element(3, 1) * m.s5 - element(3, 2) * m.s4 + element(3, 3) * m.s3) * id,
                 (-element(3, 0) * m.s5 + element(3, 2) * m.s2 - element(3, 3) * m.s1) * id,
                 (element(3, 0) * m.s4 - element(3, 1) * m.s2 + element(3, 3) * m.s0) * id,
                 (-element(3, 0) * m.s3 + element(3, 1) * m.s1 - element(3, 2) * m.s0) * id};
    r.cols[3] = {(-element(2, 1) * m.s5 + element(2, 2) * m.s4 - element(2, 3) * m.s3) * id,
                 (element(2, 0) * m.s5 - element(2, 2) * m.s2 + element(2, 3) * m.s1) * id,
                 (-element(2, 0) * m.s4 + element(2, 1) * m.s2 - element(2, 3) * m.s0) * id,
                 (element(2, 0) * m.s3 - element(2, 1) * m.s1 + element(2, 2) * m.s0) * id};
    return r;
}

} // namespace midday::math
