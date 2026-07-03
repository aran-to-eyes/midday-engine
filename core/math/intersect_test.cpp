// math.intersect.* — geometric queries: ray-AABB/sphere/triangle, AABB
// overlap semantics (closed interval), plane classification.

#include "core/math/intersect.h"
#include "doctest/doctest.h"

using namespace midday::math;

TEST_CASE("math.intersect: aabb basics — center/extents, contains, merge") {
    const Aabb box{{-1, -2, -3}, {1, 2, 3}};
    CHECK(box.center() == Vec3{0, 0, 0});
    CHECK(box.extents() == Vec3{1, 2, 3});
    CHECK(box.size() == Vec3{2, 4, 6});
    CHECK(box.contains({0, 0, 0}));
    CHECK(box.contains({1, 2, 3})); // boundary is inside (closed)
    CHECK_FALSE(box.contains({1.001f, 0, 0}));
    CHECK(Aabb::from_center_extents({5, 5, 5}, {1, 1, 1}) == Aabb{{4, 4, 4}, {6, 6, 6}});
    const Aabb merged = box.merged({{0, 0, 0}, {9, 9, 9}});
    CHECK(merged == Aabb{{-1, -2, -3}, {9, 9, 9}});
    CHECK(box.expanded({0, 0, -10}) == Aabb{{-1, -2, -10}, {1, 2, 3}});
}

TEST_CASE("math.intersect: aabb-aabb overlap is closed-interval (touching counts)") {
    const Aabb a{{0, 0, 0}, {1, 1, 1}};
    CHECK(overlaps(a, {{0.5f, 0.5f, 0.5f}, {2, 2, 2}}));
    CHECK(overlaps(a, {{1, 0, 0}, {2, 1, 1}})); // exactly touching faces
    CHECK_FALSE(overlaps(a, {{1.0001f, 0, 0}, {2, 1, 1}}));
    CHECK(overlaps(a, {{-1, -1, -1}, {2, 2, 2}})); // containment
    CHECK_FALSE(overlaps(a, {{0, 0, 2}, {1, 1, 3}}));
}

TEST_CASE("math.intersect: ray-aabb — hit, miss, inside, axis-parallel") {
    const Aabb box{{-1, -1, -1}, {1, 1, 1}};
    float t0 = -1.0f;
    float t1 = -1.0f;

    CHECK(ray_aabb({{-5, 0, 0}, {1, 0, 0}}, box, t0, t1));
    CHECK(t0 == 4.0f);
    CHECK(t1 == 6.0f);

    // From inside: interval starts at the origin.
    CHECK(ray_aabb({{0, 0, 0}, {1, 0, 0}}, box, t0, t1));
    CHECK(t0 == 0.0f);
    CHECK(t1 == 1.0f);

    // Behind the origin: no hit.
    CHECK_FALSE(ray_aabb({{5, 0, 0}, {1, 0, 0}}, box, t0, t1));
    // Parallel to a slab, outside it (exercises the inf slab path).
    CHECK_FALSE(ray_aabb({{-5, 2, 0}, {1, 0, 0}}, box, t0, t1));
    // Parallel to a slab, inside it.
    CHECK(ray_aabb({{-5, 0.5f, 0.5f}, {1, 0, 0}}, box, t0, t1));
    // Diagonal hit, negative direction components.
    CHECK(ray_aabb({{3, 3, 3}, {-1, -1, -1}}, box, t0, t1));
    CHECK(t0 == 2.0f);
    CHECK(t1 == 4.0f);
}

TEST_CASE("math.intersect: ray-sphere — outside, inside, behind, tangent-miss") {
    const Sphere s{{0, 0, 0}, 1.0f};
    float t = -1.0f;

    CHECK(ray_sphere({{-3, 0, 0}, {1, 0, 0}}, s, t));
    CHECK(t == 2.0f);

    // Origin inside: reports the exit point.
    CHECK(ray_sphere({{0, 0, 0}, {1, 0, 0}}, s, t));
    CHECK(t == 1.0f);

    CHECK_FALSE(ray_sphere({{3, 0, 0}, {1, 0, 0}}, s, t));  // pointing away
    CHECK_FALSE(ray_sphere({{-3, 2, 0}, {1, 0, 0}}, s, t)); // clean miss
    // Unnormalized direction: t is in direction-length units.
    CHECK(ray_sphere({{-4, 0, 0}, {2, 0, 0}}, s, t));
    CHECK(t == 1.5f);
}

TEST_CASE("math.intersect: ray-triangle — hit with barycentrics, miss, parallel") {
    const Vec3 v0{-1, -1, 0};
    const Vec3 v1{1, -1, 0};
    const Vec3 v2{0, 1, 0};
    float t = -1.0f;
    float u = -1.0f;
    float v = -1.0f;

    CHECK(ray_triangle({{0, -0.2f, -3}, {0, 0, 1}}, v0, v1, v2, t, u, v));
    CHECK(t == 3.0f);
    CHECK(u >= 0.0f);
    CHECK(v >= 0.0f);
    CHECK(u + v <= 1.0f);

    // Hitting exactly vertex v1: u = 1, v = 0.
    CHECK(ray_triangle({{1, -1, 5}, {0, 0, -1}}, v0, v1, v2, t, u, v));
    CHECK(t == 5.0f);
    CHECK(almost_equal(u, 1.0f));
    CHECK(almost_equal(v, 0.0f));

    // No backface culling: approaching from either side hits.
    CHECK(ray_triangle({{0, -0.2f, 3}, {0, 0, -1}}, v0, v1, v2, t, u, v));

    CHECK_FALSE(ray_triangle({{5, 5, -3}, {0, 0, 1}}, v0, v1, v2, t, u, v));      // outside
    CHECK_FALSE(ray_triangle({{0, 0, -3}, {1, 0, 0}}, v0, v1, v2, t, u, v));      // parallel
    CHECK_FALSE(ray_triangle({{0, -0.2f, -3}, {0, 0, -1}}, v0, v1, v2, t, u, v)); // behind
}

TEST_CASE("math.intersect: plane — signed distance and point classification") {
    const Plane p = Plane::from_point_normal({0, 2, 0}, {0, 1, 0});
    CHECK(p.d == 2.0f);
    CHECK(p.signed_distance({0, 5, 0}) == 3.0f);
    CHECK(p.signed_distance({7, -1, 3}) == -3.0f);
    CHECK(classify(p, Vec3{0, 5, 0}) == PlaneSide::Front);
    CHECK(classify(p, Vec3{0, -5, 0}) == PlaneSide::Back);
    CHECK(classify(p, Vec3{9, 2, -4}) == PlaneSide::On);

    // from_points: CCW winding faces +z (right-handed).
    const Plane tri = Plane::from_points({0, 0, 1}, {1, 0, 1}, {0, 1, 1});
    CHECK(almost_equal(tri.normal.z, 1.0f));
    CHECK(almost_equal(tri.d, 1.0f));
}

TEST_CASE("math.intersect: plane vs aabb classification (center-extent radius)") {
    const Plane p = Plane::from_point_normal({0, 0, 0}, {0, 1, 0});
    CHECK(classify(p, Aabb{{-1, 2, -1}, {1, 4, 1}}) == PlaneSide::Front);
    CHECK(classify(p, Aabb{{-1, -4, -1}, {1, -2, 1}}) == PlaneSide::Back);
    CHECK(classify(p, Aabb{{-1, -1, -1}, {1, 1, 1}}) == PlaneSide::On); // straddles
    CHECK(classify(p, Aabb{{-1, 0, -1}, {1, 2, 1}}) == PlaneSide::On);  // touches
}
