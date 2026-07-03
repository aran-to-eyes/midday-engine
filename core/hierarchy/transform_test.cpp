// hierarchy.transform — nested local TRS composes to known-answer world
// matrices (matrix-space composition, core/math/xform.h policy); dirty
// flags recompute only what changed (untouched subtrees stay bit-identical);
// reparent keeps the LOCAL transform (core/hierarchy/transforms.cpp).

#include "core/ecs/world.h"
#include "core/hierarchy/hierarchy.h"
#include "core/math/mat.h"
#include "core/math/quat.h"
#include "core/math/vec.h"
#include "core/math/xform.h"
#include "core/reflect/registry.h"
#include "testkit/doctest.h"

#include <cmath>

using midday::ecs::EntityRef;
using midday::ecs::World;
using midday::hierarchy::Hierarchy;
using midday::math::Mat4;
using midday::math::Quat;
using midday::math::Transform;
using midday::math::Vec3;
using midday::reflect::Registry;

namespace {

void check_mat4_approx(const Mat4& actual, const Mat4& expected) {
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row)
            CHECK(actual.element(row, col) ==
                  doctest::Approx(expected.element(row, col)).epsilon(1e-5));
    }
}

} // namespace

TEST_CASE("hierarchy.transform: nested locals compose to known-answer world matrices") {
    Registry registry;
    World world(registry);
    Hierarchy hierarchy(world);

    const EntityRef root = world.spawn();
    const EntityRef child = world.spawn();
    const EntityRef grandchild = world.spawn();
    REQUIRE_FALSE(hierarchy.adopt(root));
    REQUIRE_FALSE(hierarchy.queue_attach(child, root));
    REQUIRE_FALSE(hierarchy.queue_attach(grandchild, child));
    REQUIRE_FALSE(world.flush_structural());

    // root: translate (10,0,0), rotate 90 degrees about Z (unit quat built
    // from sqrt — bit-portable, no libm); child: translate (5,0,0), scale 2;
    // grandchild: translate (1,1,0).
    const float k = std::sqrt(0.5f);
    REQUIRE_FALSE(hierarchy.set_local(root, Transform{{10, 0, 0}, Quat{0, 0, k, k}, {1, 1, 1}}));
    REQUIRE_FALSE(hierarchy.set_local(child, Transform{{5, 0, 0}, Quat{}, {2, 2, 2}}));
    REQUIRE_FALSE(hierarchy.set_local(grandchild, Transform{{1, 1, 0}, Quat{}, {1, 1, 1}}));
    hierarchy.propagate();

    // Hand-derived fixtures. Rz(90): x-axis -> +y, y-axis -> -x.
    const Mat4 expected_root =
        Mat4::from_cols({0, 1, 0, 0}, {-1, 0, 0, 0}, {0, 0, 1, 0}, {10, 0, 0, 1});
    // child world: linear = Rz90 * 2I; translation = Rz90*(5,0,0) + (10,0,0).
    const Mat4 expected_child =
        Mat4::from_cols({0, 2, 0, 0}, {-2, 0, 0, 0}, {0, 0, 2, 0}, {10, 5, 0, 1});
    // grandchild world: same linear; translation = childW * (1,1,0)
    //                 = (0,2,0) + (-2,0,0) + (10,5,0) = (8,7,0).
    const Mat4 expected_grandchild =
        Mat4::from_cols({0, 2, 0, 0}, {-2, 0, 0, 0}, {0, 0, 2, 0}, {8, 7, 0, 1});

    check_mat4_approx(*hierarchy.world_of(root), expected_root);
    check_mat4_approx(*hierarchy.world_of(child), expected_child);
    check_mat4_approx(*hierarchy.world_of(grandchild), expected_grandchild);

    // A root's world IS its local matrix.
    Registry registry2;
    World world2(registry2);
    Hierarchy hierarchy2(world2);
    const EntityRef lone = world2.spawn();
    REQUIRE_FALSE(hierarchy2.adopt(lone));
    const Transform t{{1, 2, 3}, Quat{}, {4, 5, 6}};
    REQUIRE_FALSE(hierarchy2.set_local(lone, t));
    hierarchy2.propagate();
    CHECK(*hierarchy2.world_of(lone) == t.to_mat4());
}

TEST_CASE("hierarchy.transform: dirty flags leave untouched subtrees bit-identical") {
    Registry registry;
    World world(registry);
    Hierarchy hierarchy(world);

    // r -> (a -> a1, b -> b1): edit only a1; b's line must not be recomputed
    // differently — bit-identical matrices prove the clean path is untouched.
    const EntityRef r = world.spawn();
    const EntityRef a = world.spawn();
    const EntityRef a1 = world.spawn();
    const EntityRef b = world.spawn();
    const EntityRef b1 = world.spawn();
    REQUIRE_FALSE(hierarchy.adopt(r));
    REQUIRE_FALSE(hierarchy.queue_attach(a, r));
    REQUIRE_FALSE(hierarchy.queue_attach(a1, a));
    REQUIRE_FALSE(hierarchy.queue_attach(b, r));
    REQUIRE_FALSE(hierarchy.queue_attach(b1, b));
    REQUIRE_FALSE(world.flush_structural());
    REQUIRE_FALSE(hierarchy.set_local(r, Transform{{1, 0, 0}, Quat{}, {1, 1, 1}}));
    REQUIRE_FALSE(hierarchy.set_local(b, Transform{{0, 3, 0}, Quat{}, {1, 1, 1}}));
    REQUIRE_FALSE(hierarchy.set_local(b1, Transform{{0, 0, 7}, Quat{}, {1, 1, 1}}));
    hierarchy.propagate();

    const Mat4 b_before = *hierarchy.world_of(b);
    const Mat4 b1_before = *hierarchy.world_of(b1);
    const Mat4 r_before = *hierarchy.world_of(r);

    REQUIRE_FALSE(hierarchy.set_local(a1, Transform{{9, 9, 9}, Quat{}, {1, 1, 1}}));
    hierarchy.propagate();

    CHECK(*hierarchy.world_of(b) == b_before);   // exact — not Approx
    CHECK(*hierarchy.world_of(b1) == b1_before); // exact
    CHECK(*hierarchy.world_of(r) == r_before);   // exact
    CHECK(hierarchy.world_of(a1)->element(0, 3) == doctest::Approx(10.0));
}

TEST_CASE("hierarchy.transform: reparent keeps local, world recomposes in the new frame") {
    Registry registry;
    World world(registry);
    Hierarchy hierarchy(world);

    const EntityRef home = world.spawn();
    const EntityRef away = world.spawn();
    const EntityRef mover = world.spawn();
    REQUIRE_FALSE(hierarchy.adopt(home));
    REQUIRE_FALSE(hierarchy.adopt(away));
    REQUIRE_FALSE(hierarchy.queue_attach(mover, home));
    REQUIRE_FALSE(world.flush_structural());
    REQUIRE_FALSE(hierarchy.set_local(home, Transform{{10, 0, 0}, Quat{}, {1, 1, 1}}));
    REQUIRE_FALSE(hierarchy.set_local(away, Transform{{100, 0, 0}, Quat{}, {1, 1, 1}}));
    REQUIRE_FALSE(hierarchy.set_local(mover, Transform{{5, 0, 0}, Quat{}, {1, 1, 1}}));
    hierarchy.propagate();
    CHECK(hierarchy.world_of(mover)->element(0, 3) == doctest::Approx(15.0));

    REQUIRE_FALSE(hierarchy.queue_attach(mover, away));
    REQUIRE_FALSE(world.flush_structural());
    hierarchy.propagate();
    CHECK(hierarchy.local_of(mover)->translation == Vec3{5, 0, 0}); // local preserved
    CHECK(hierarchy.world_of(mover)->element(0, 3) == doctest::Approx(105.0));
}
