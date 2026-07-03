// math.xform.* — TRS compose order and the deterministic decompose policy.

#include "core/math/xform.h"
#include "doctest/doctest.h"

using namespace midday::math;

namespace {

bool vec3_almost_equal(Vec3 a, Vec3 b, float eps = 1e-5f) {
    return almost_equal(a.x, b.x, eps) && almost_equal(a.y, b.y, eps) &&
           almost_equal(a.z, b.z, eps);
}

bool same_rotation(Quat a, Quat b, float eps = 1e-5f) {
    const float d = dot(a, b);
    return almost_equal(d < 0.0f ? -d : d, 1.0f, eps);
}

} // namespace

TEST_CASE("math.xform: compose order is scale, then rotate, then translate") {
    Transform trs;
    trs.translation = {10, 0, 0};
    trs.rotation = Quat::from_axis_angle({0, 0, 1}, kPi * 0.5f);
    trs.scale = {2, 1, 1};
    // (1,0,0) -> scale -> (2,0,0) -> rotate z90 -> (0,2,0) -> translate -> (10,2,0).
    CHECK(vec3_almost_equal(trs.to_mat4().transform_point({1, 0, 0}), {10, 2, 0}));
    // Equals the explicit T * R * S matrix product.
    const Mat4 explicit_product = Mat4::translation(trs.translation) *
                                  Mat4::from_mat3(trs.rotation.to_mat3()) *
                                  Mat4::scaling(trs.scale);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            CHECK(almost_equal(trs.to_mat4().element(r, c), explicit_product.element(r, c)));
}

TEST_CASE("math.xform: identity round trip is exact") {
    const Transform identity = Transform::identity();
    CHECK(identity.to_mat4() == Mat4::identity());
    CHECK(Transform::from_mat4(Mat4::identity()) == identity);
}

TEST_CASE("math.xform: decompose recovers TRS components") {
    Transform trs;
    trs.translation = {-3, 7, 0.5f};
    trs.rotation = Quat::from_axis_angle(Vec3{1, -2, 0.5f}.normalized(), 1.3f);
    trs.scale = {2, 0.5f, 3};
    const Transform back = Transform::from_mat4(trs.to_mat4());
    CHECK(vec3_almost_equal(back.translation, trs.translation));
    CHECK(vec3_almost_equal(back.scale, trs.scale));
    CHECK(same_rotation(back.rotation, trs.rotation));
    // Decomposition reproduces the matrix.
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            CHECK(almost_equal(back.to_mat4().element(r, c), trs.to_mat4().element(r, c)));
}

TEST_CASE("math.xform: mirror convention folds negative determinant into scale.x") {
    const Mat4 mirror = Mat4::scaling({-2, 3, 4});
    const Transform back = Transform::from_mat4(mirror);
    CHECK(back.scale.x == -2.0f); // THE convention: x carries the mirror
    CHECK(back.scale.y == 3.0f);
    CHECK(back.scale.z == 4.0f);
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            CHECK(almost_equal(back.to_mat4().element(r, c), mirror.element(r, c)));
}

TEST_CASE("math.xform: degenerate scale decomposes without NaN") {
    const Mat4 flat = Mat4::scaling({2, 0, 3});
    const Transform back = Transform::from_mat4(flat);
    CHECK(back.scale.y == 0.0f);
    CHECK(back.rotation.is_normalized()); // fell back to a canonical axis
    CHECK(back.translation == Vec3{});
}

TEST_CASE("math.xform: decompose is bit-deterministic across repeat calls") {
    Transform trs;
    trs.translation = {1.25f, -9.5f, 3.75f};
    trs.rotation = Quat::from_axis_angle(Vec3{0.3f, 1.0f, -0.2f}.normalized(), 2.9f);
    trs.scale = {1.5f, 2.5f, 0.75f};
    const Mat4 m = trs.to_mat4();
    const Transform a = Transform::from_mat4(m);
    const Transform b = Transform::from_mat4(m);
    CHECK(a == b); // exact bit equality, not approx
}
