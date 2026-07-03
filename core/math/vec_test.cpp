// math.vec.* — Vec2/Vec3/Vec4 arithmetic, dot/cross, normalize policy.

#include "core/math/vec.h"
#include "testkit/doctest.h"

using namespace midday::math;

TEST_CASE("math.vec: arithmetic is component-wise, operator* is Hadamard") {
    const Vec3 a{1, 2, 3};
    const Vec3 b{4, -5, 6};
    CHECK(a + b == Vec3{5, -3, 9});
    CHECK(a - b == Vec3{-3, 7, -3});
    CHECK(a * b == Vec3{4, -10, 18});
    CHECK(a * 2.0f == Vec3{2, 4, 6});
    CHECK(2.0f * a == a * 2.0f);
    CHECK(b / 2.0f == Vec3{2, -2.5f, 3});
    CHECK(-a == Vec3{-1, -2, -3});
    CHECK(Vec2{1, 2} + Vec2{3, 4} == Vec2{4, 6});
    CHECK(Vec4{1, 2, 3, 4} + Vec4{1, 1, 1, 1} == Vec4{2, 3, 4, 5});
}

TEST_CASE("math.vec: value-init is the zero vector") {
    CHECK(Vec2{} == Vec2{0, 0});
    CHECK(Vec3{} == Vec3{0, 0, 0});
    CHECK(Vec4{} == Vec4{0, 0, 0, 0});
}

TEST_CASE("math.vec: dot and cross follow the right-handed convention") {
    CHECK(dot(Vec3{1, 2, 3}, Vec3{4, 5, 6}) == 32.0f);
    CHECK(cross(Vec3{1, 0, 0}, Vec3{0, 1, 0}) == Vec3{0, 0, 1}); // x cross y = z
    CHECK(cross(Vec3{0, 1, 0}, Vec3{0, 0, 1}) == Vec3{1, 0, 0});
    CHECK(cross(Vec2{1, 0}, Vec2{0, 1}) == 1.0f);
    CHECK(dot(Vec2{1, 2}, Vec2{3, 4}) == 11.0f);
    CHECK(dot(Vec4{1, 2, 3, 4}, Vec4{4, 3, 2, 1}) == 20.0f);
}

TEST_CASE("math.vec: length and normalization") {
    CHECK(Vec3{3, 4, 0}.length() == 5.0f);
    CHECK(Vec3{3, 4, 0}.length_squared() == 25.0f);
    const Vec3 n = Vec3{0, 0, 7}.normalized();
    CHECK(n == Vec3{0, 0, 1});
    // Policy: zero normalizes to zero, never NaN.
    CHECK(Vec3{}.normalized() == Vec3{});
    CHECK(Vec2{}.normalized() == Vec2{});
    CHECK(Vec3{}.normalized_or(Vec3{0, 1, 0}) == Vec3{0, 1, 0});
    CHECK(Vec3{2, 0, 0}.normalized_or(Vec3{0, 1, 0}) == Vec3{1, 0, 0});
}

TEST_CASE("math.vec: lerp, clamp, min/max/abs helpers") {
    CHECK(lerp(0.0f, 10.0f, 0.25f) == 2.5f);
    CHECK(lerp(Vec3{0, 0, 0}, Vec3{2, 4, 8}, 0.5f) == Vec3{1, 2, 4});
    CHECK(lerp(Vec2{0, 0}, Vec2{4, 8}, 0.75f) == Vec2{3, 6});
    CHECK(clamp(5.0f, 0.0f, 1.0f) == 1.0f);
    CHECK(saturate(-2.0f) == 0.0f);
    CHECK(vec_min(Vec3{1, 5, 3}, Vec3{2, 4, 3}) == Vec3{1, 4, 3});
    CHECK(vec_max(Vec3{1, 5, 3}, Vec3{2, 4, 3}) == Vec3{2, 5, 3});
    CHECK(vec_abs(Vec3{-1, 2, -3}) == Vec3{1, 2, 3});
}

TEST_CASE("math.vec: indexing matches component order") {
    const Vec4 v{10, 20, 30, 40};
    CHECK(v[0] == 10.0f);
    CHECK(v[1] == 20.0f);
    CHECK(v[2] == 30.0f);
    CHECK(v[3] == 40.0f);
    CHECK(v.xyz() == Vec3{10, 20, 30});
    Vec3 m{1, 2, 3};
    m[1] = 9.0f;
    CHECK(m == Vec3{1, 9, 3});
}
