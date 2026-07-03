// math.mat.* — THE column-convention assertions (mat.h documents the
// convention once; these tests pin it) plus products, inverses, determinants.

#include "core/math/mat.h"
#include "testkit/doctest.h"

using namespace midday::math;

namespace {

bool mat4_almost_equal(const Mat4& a, const Mat4& b, float eps = 1e-5f) {
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            if (!almost_equal(a.element(r, c), b.element(r, c), eps))
                return false;
    return true;
}

} // namespace

TEST_CASE("math.mat: THE column convention — column-major storage, column vectors") {
    // Translation lives in COLUMN 3.
    const Mat4 t = Mat4::translation({10, 20, 30});
    CHECK(t.cols[3] == Vec4{10, 20, 30, 1});
    CHECK(t.element(0, 3) == 10.0f); // row 0, col 3
    CHECK(t.element(3, 3) == 1.0f);
    CHECK(t.element(3, 0) == 0.0f); // bottom row is 0 0 0 1

    // v' = M * v: translating the origin point yields the translation.
    CHECK(t.transform_point({0, 0, 0}) == Vec3{10, 20, 30});
    // Directions ignore translation (w = 0).
    CHECK(t.transform_vector({1, 2, 3}) == Vec3{1, 2, 3});

    // element(r, c) == cols[c][r].
    Mat4 m;
    m.cols[1] = {5, 6, 7, 8};
    CHECK(m.element(0, 1) == 5.0f);
    CHECK(m.element(3, 1) == 8.0f);

    // Composition is right-to-left: (A * B) applies B first.
    const Mat4 scale = Mat4::scaling({2, 2, 2});
    const Vec3 p = (t * scale).transform_point({1, 0, 0}); // scale, THEN translate
    CHECK(p == Vec3{12, 20, 30});
    const Vec3 q = (scale * t).transform_point({1, 0, 0}); // translate, THEN scale
    CHECK(q == Vec3{22, 40, 60});
}

TEST_CASE("math.mat: identity, default construction, equality") {
    CHECK(Mat4{} == Mat4::identity());
    CHECK(Mat3{} == Mat3::identity());
    const Vec4 v{1, 2, 3, 4};
    CHECK(Mat4::identity() * v == v);
    CHECK(Mat3::identity() * Vec3{1, 2, 3} == Vec3{1, 2, 3});
}

TEST_CASE("math.mat: transpose swaps element(r,c) and element(c,r)") {
    const Mat4 m = Mat4::from_cols({1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}, {13, 14, 15, 16});
    const Mat4 mt = m.transposed();
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            CHECK(mt.element(r, c) == m.element(c, r));
    CHECK(mat4_almost_equal(mt.transposed(), m, 0.0f));
}

TEST_CASE("math.mat: Mat3 determinant and inverse") {
    CHECK(Mat3::identity().determinant() == 1.0f);
    CHECK(Mat3::scaling({2, 3, 4}).determinant() == 24.0f);
    const Mat3 m = Mat3::from_cols({2, 0, 1}, {0, 3, 0}, {1, 0, 2});
    const Mat3 inv = m.inverse();
    const Mat3 product = m * inv;
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            CHECK(almost_equal(product.element(r, c), r == c ? 1.0f : 0.0f));
    // Mirror matrix: negative determinant.
    CHECK(Mat3::scaling({-1, 1, 1}).determinant() == -1.0f);
}

TEST_CASE("math.mat: Mat4 determinant and general inverse") {
    CHECK(Mat4::identity().determinant() == 1.0f);
    CHECK(Mat4::scaling({2, 3, 4}).determinant() == 24.0f);
    CHECK(Mat4::translation({7, 8, 9}).determinant() == 1.0f);

    // A full-rank matrix with no special structure.
    const Mat4 m = Mat4::from_cols({4, 0, 1, 0}, {2, 5, 0, 1}, {0, 1, 3, 0}, {1, 0, 2, 6});
    const float det = m.determinant();
    CHECK(det != 0.0f);
    CHECK(mat4_almost_equal(m * m.inverse(), Mat4::identity()));
    CHECK(mat4_almost_equal(m.inverse() * m, Mat4::identity()));
}

TEST_CASE("math.mat: affine inverse matches the general inverse on affine input") {
    const Mat4 affine = Mat4::translation({3, -2, 5}) * Mat4::scaling({2, 4, 0.5f});
    CHECK(mat4_almost_equal(affine.inverse_affine(), affine.inverse()));
    const Vec3 p{1, 2, 3};
    const Vec3 round_trip = affine.inverse_affine().transform_point(affine.transform_point(p));
    CHECK(almost_equal(round_trip.x, p.x));
    CHECK(almost_equal(round_trip.y, p.y));
    CHECK(almost_equal(round_trip.z, p.z));
}

TEST_CASE("math.mat: upper3x3 / from_mat3 embed round trip") {
    const Mat3 linear = Mat3::from_cols({1, 2, 3}, {4, 5, 6}, {7, 8, 10});
    const Mat4 embedded = Mat4::from_mat3(linear, {11, 12, 13});
    CHECK(embedded.upper3x3() == linear);
    CHECK(embedded.cols[3] == Vec4{11, 12, 13, 1});
    CHECK(embedded.element(3, 0) == 0.0f);
    CHECK(embedded.element(3, 1) == 0.0f);
    CHECK(embedded.element(3, 2) == 0.0f);
}
