// core/physics/physics_server_test.cpp — physics.min.* server surface:
// the deterministic config lock (THE exit test), body create/destroy
// refusals, and gravity + transform sync through the real tick loop.

#include "core/physics/physics_server.h"
#include "core/physics/test_support.h"

namespace midday::physics {
namespace {

using test::PhysicsFixture;
using test::unwrap;

TEST_CASE("physics.min.config_lock: deterministic mode refuses divergent thread config") {
    testkit::SimFixture sim;

    // Construction with a divergent thread count: structured refusal, pinned.
    PhysicsConfig divergent;
    divergent.threads = 4;
    auto refused = PhysicsServer::create(sim.world, sim.hierarchy, sim.bus(), divergent);
    REQUIRE(refused.server == nullptr);
    const base::Error& refusal = unwrap(refused.error);
    CHECK(refusal.code == "physics.config_locked");
    CHECK(test::field(refusal.details, "mode").as_string() == "deterministic");
    CHECK(test::field(refusal.details, "threads").as_int() == 4);
    CHECK(test::field(refusal.details, "required_threads").as_int() == 1);

    // Non-deterministic multi-threading is an M0 scope refusal — a
    // deliberately DISTINCT code from the determinism lock.
    PhysicsConfig play;
    play.deterministic = false;
    play.threads = 8;
    auto unsupported = PhysicsServer::create(sim.world, sim.hierarchy, sim.bus(), play);
    REQUIRE(unsupported.server == nullptr);
    CHECK(unwrap(unsupported.error).code == "physics.threads_unsupported");

    // The pinned config constructs; reconfiguring away from it refuses.
    auto created = PhysicsServer::create(sim.world, sim.hierarchy, sim.bus(), {});
    REQUIRE_FALSE(created.error.has_value());
    PhysicsServer& server = *created.server;
    CHECK_FALSE(server.reconfigure_threads(1).has_value()); // same config: no-op
    const auto locked = server.reconfigure_threads(2);
    const base::Error& lock_error = unwrap(locked);
    CHECK(lock_error.code == "physics.config_locked");
    CHECK(test::field(lock_error.details, "threads").as_int() == 2);
    CHECK(test::field(lock_error.details, "current_threads").as_int() == 1);
}

TEST_CASE("physics.min.bodies: create/destroy with structured refusals") {
    PhysicsFixture fixture;
    PhysicsServer& server = *fixture.server;
    ecs::World& world = fixture.sim.world;

    // Unadopted entity: no sync target, refused.
    const ecs::EntityRef unadopted = world.spawn();
    base::Error error;
    CHECK(server.create_dynamic_box(unadopted, {0.5F, 0.5F, 0.5F}, {}, &error).is_null());
    CHECK(error.code == "physics.not_adopted");

    // Dead entity: the ECS refusal passes through.
    ecs::EntityRef dead = world.spawn();
    REQUIRE_FALSE(world.despawn(dead).has_value());
    CHECK(server.create_dynamic_box(dead, {0.5F, 0.5F, 0.5F}, {}, &error).is_null());
    CHECK(error.code == "ecs.stale_handle");

    // Degenerate shape.
    const ecs::EntityRef flat = fixture.adopted_entity();
    CHECK(server.create_dynamic_box(flat, {0.5F, 0.0F, 0.5F}, {}, &error).is_null());
    CHECK(error.code == "physics.bad_shape");

    // The happy path: box + ground, bridged through the PhysicsBody row.
    const ecs::EntityRef ground = fixture.adopted_entity();
    const PhysicsBodyId ground_id = server.create_ground_plane(ground, 0.0F, &error);
    REQUIRE_FALSE(ground_id.is_null());
    const ecs::EntityRef box = fixture.drop_box({0.0F, 3.0F, 0.0F});
    const PhysicsBodyId box_id = server.body_of(box);
    REQUIRE_FALSE(box_id.is_null());
    CHECK(server.body_count() == 2);
    CHECK(server.entity_of(box_id) == box);
    CHECK(world.try_get<PhysicsBody>(box)->id == box_id);

    // One body per entity.
    CHECK(server.create_dynamic_box(box, {0.5F, 0.5F, 0.5F}, {}, &error).is_null());
    CHECK(error.code == "physics.duplicate_body");

    // Destroy: row nulls + deactivates; second destroy refuses; rebind works.
    REQUIRE_FALSE(server.destroy_body(box).has_value());
    CHECK(server.body_of(box).is_null());
    CHECK(server.body_count() == 1);
    CHECK(world.is_active<PhysicsBody>(box) == std::optional<bool>{false});
    const auto missing = server.destroy_body(box);
    CHECK(unwrap(missing).code == "physics.no_body");
    math::Transform at;
    at.translation = {0.0F, 2.0F, 0.0F};
    CHECK_FALSE(server.create_dynamic_box(box, {0.5F, 0.5F, 0.5F}, at, &error).is_null());
    CHECK(server.body_count() == 2);
}

TEST_CASE("physics.min.gravity_sync: boxes fall; sync writes hierarchy locals, keeps scale") {
    PhysicsFixture fixture;
    PhysicsServer& server = *fixture.server;
    hierarchy::Hierarchy& hierarchy = fixture.sim.hierarchy;

    CHECK(server.gravity().y == -9.81F);

    const ecs::EntityRef ground = fixture.adopted_entity();
    REQUIRE_FALSE(server.create_ground_plane(ground, 0.0F).is_null());

    // A box with authored scale: physics must move it and keep the scale.
    const ecs::EntityRef box = fixture.adopted_entity();
    math::Transform start;
    start.translation = {0.0F, 5.0F, 0.0F};
    start.scale = {2.0F, 2.0F, 2.0F};
    base::Error error;
    const PhysicsBodyId id = server.create_dynamic_box(box, {0.5F, 0.5F, 0.5F}, start, &error);
    REQUIRE_FALSE(id.is_null());
    REQUIRE(hierarchy.local_of(box) != nullptr);
    CHECK(hierarchy.local_of(box)->translation.y == 5.0F); // create writes the local

    REQUIRE_FALSE(fixture.sim.loop().tick(30).has_value());

    const math::Transform* local = hierarchy.local_of(box);
    REQUIRE(local != nullptr);
    CHECK(local->translation.y < 5.0F); // gravity acted
    CHECK(local->translation.y > 0.0F); // and the ground holds
    CHECK(local->scale.x == 2.0F);      // authored scale preserved
    CHECK(server.stats().steps == 30);
    CHECK(server.stats().sync_writes == 30); // one dynamic body, 30 ticks

    // The synced local IS the body's world transform (bodies are roots).
    const auto body = server.body_transform(id);
    CHECK(unwrap(body).translation.y == local->translation.y);

    // Phase 8 propagated the world matrix from the synced local.
    const math::Mat4* world_matrix = hierarchy.world_of(box);
    REQUIRE(world_matrix != nullptr);
    CHECK(world_matrix->cols[3].y == local->translation.y);
}

} // namespace
} // namespace midday::physics
