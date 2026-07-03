// math.quat.* — normalize policy, Hamilton product vs matrix product,
// rotate vs to_mat3, from_mat3 across all four Shepperd branches.

#include "core/math/quat.h"
#include "testkit/doctest.h"

#include <cmath>

using namespace midday::math;

namespace {

bool vec3_almost_equal(Vec3 a, Vec3 b, float eps = 1e-5f) {
    return almost_equal(a.x, b.x, eps) && almost_equal(a.y, b.y, eps) &&
           almost_equal(a.z, b.z, eps);
}

// Same rotation allowing the q / -q double cover.
bool same_rotation(Quat a, Quat b, float eps = 1e-5f) {
    const float d = dot(a, b);
    return almost_equal(d < 0.0f ? -d : d, 1.0f, eps);
}

} // namespace

TEST_CASE("math.quat: identity and normalize policy") {
    CHECK(Quat{} == Quat::identity());
    CHECK(Quat::identity().is_normalized());
    CHECK(Quat::identity().rotate({1, 2, 3}) == Vec3{1, 2, 3});
    // Zero quaternion normalizes to identity, never NaN.
    CHECK(Quat{0, 0, 0, 0}.normalized() == Quat::identity());
    const Quat q = Quat{1, 2, 3, 4}.normalized();
    CHECK(q.is_normalized());
    CHECK_FALSE(Quat{1, 2, 3, 4}.is_normalized());
}

TEST_CASE("math.quat: axis-angle quarter turns rotate the basis") {
    const Quat z90 = Quat::from_axis_angle({0, 0, 1}, kPi * 0.5f);
    CHECK(z90.is_normalized());
    CHECK(vec3_almost_equal(z90.rotate({1, 0, 0}), {0, 1, 0})); // right-handed
    const Quat x90 = Quat::from_axis_angle({1, 0, 0}, kPi * 0.5f);
    CHECK(vec3_almost_equal(x90.rotate({0, 1, 0}), {0, 0, 1}));
}

TEST_CASE("math.quat: product order matches matrix product (b first)") {
    const Quat a = Quat::from_axis_angle({0, 0, 1}, 0.7f);
    const Quat b = Quat::from_axis_angle({1, 0, 0}, -1.2f);
    const Vec3 v{0.3f, -2.0f, 1.5f};
    CHECK(vec3_almost_equal((a * b).rotate(v), a.rotate(b.rotate(v))));
    // to_mat3 is a homomorphism: mat(a * b) == mat(a) * mat(b).
    const Mat3 lhs = (a * b).to_mat3();
    const Mat3 rhs = a.to_mat3() * b.to_mat3();
    for (int c = 0; c < 3; ++c)
        CHECK(vec3_almost_equal(lhs.cols[c], rhs.cols[c]));
}

TEST_CASE("math.quat: rotate agrees with to_mat3, inverse undoes") {
    const Quat q = Quat::from_axis_angle(Vec3{1, 2, -1}.normalized(), 2.1f);
    const Vec3 v{-4, 0.5f, 2};
    CHECK(vec3_almost_equal(q.rotate(v), q.to_mat3() * v));
    CHECK(vec3_almost_equal(q.inverse().rotate(q.rotate(v)), v));
    // Rotation preserves length.
    CHECK(almost_equal(q.rotate(v).length(), v.length()));
}

TEST_CASE("math.quat: from_mat3 round trip hits all four extraction branches") {
    // trace > 0 (small rotation), then one dominant diagonal element each.
    const Quat cases[] = {
        Quat::from_axis_angle(Vec3{1, 1, 1}.normalized(), 0.3f), // trace-major
        Quat::from_axis_angle({1, 0, 0}, 3.0f),                  // x-major
        Quat::from_axis_angle({0, 1, 0}, 3.0f),                  // y-major
        Quat::from_axis_angle({0, 0, 1}, 3.0f),                  // z-major
    };
    for (const Quat& q : cases) {
        const Quat back = Quat::from_mat3(q.to_mat3());
        CHECK(back.is_normalized());
        CHECK(same_rotation(back, q));
    }
}

TEST_CASE("math.quat: nlerp endpoints, unit output, shortest arc") {
    const Quat a = Quat::from_axis_angle({0, 1, 0}, 0.4f);
    const Quat b = Quat::from_axis_angle({0, 1, 0}, 1.8f);
    CHECK(same_rotation(nlerp(a, b, 0.0f), a));
    CHECK(same_rotation(nlerp(a, b, 1.0f), b));
    CHECK(nlerp(a, b, 0.37f).is_normalized());
    // Shortest arc: blending q with -q stays on the same rotation.
    const Quat neg{-a.x, -a.y, -a.z, -a.w};
    CHECK(same_rotation(nlerp(a, neg, 0.5f), a));
}

TEST_CASE("math.quat: slerp constant angular velocity and endpoints") {
    const Quat a = Quat::identity();
    const Quat b = Quat::from_axis_angle({0, 0, 1}, 2.0f);
    CHECK(same_rotation(slerp(a, b, 0.0f), a));
    CHECK(same_rotation(slerp(a, b, 1.0f), b));
    const Quat half = slerp(a, b, 0.5f);
    CHECK(same_rotation(half, Quat::from_axis_angle({0, 0, 1}, 1.0f)));
    // Tiny arcs take the nlerp fallback and stay unit.
    const Quat c = Quat::from_axis_angle({0, 0, 1}, 1e-4f);
    CHECK(slerp(a, c, 0.5f).is_normalized());
}
