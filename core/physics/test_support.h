// core/physics/test_support.h — shared fixtures for the physics.min*
// selftests ONLY (compiled into midday_physics_tests, never the library).
// Composition: the canonical sim fixture (testkit/sim_fixture.h) + a
// PhysicsServer hooked into tick phase 6 — exactly the boot wiring the
// engine will use.

#pragma once

#include "core/physics/physics_server.h"
#include "testkit/doctest.h"
#include "testkit/sim_fixture.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace midday::physics::test {

using testkit::code_of;
using testkit::field;
using testkit::unwrap;

struct PhysicsFixture {
    testkit::SimFixture sim;
    std::unique_ptr<PhysicsServer> server;

    explicit PhysicsFixture(PhysicsConfig config = {}, tick::TickLoopConfig loop_config = {})
        : sim(loop_config) {
        auto created = PhysicsServer::create(sim.world, sim.hierarchy, sim.bus(), config);
        REQUIRE_FALSE(created.error.has_value());
        server = std::move(created.server);
        REQUIRE(server != nullptr);
        REQUIRE_FALSE(sim.loop().add_hook(tick::Phase::kPhysics, *server).has_value());
    }

    // A live, hierarchy-adopted entity — the create_* precondition.
    ecs::EntityRef adopted_entity() {
        const ecs::EntityRef entity = sim.world.spawn();
        REQUIRE_FALSE(sim.hierarchy.adopt(entity).has_value());
        return entity;
    }

    // A dynamic unit-ish box (half extent 0.5) at world position.
    ecs::EntityRef drop_box(math::Vec3 position) {
        const ecs::EntityRef entity = adopted_entity();
        math::Transform transform;
        transform.translation = position;
        base::Error error;
        const PhysicsBodyId id =
            server->create_dynamic_box(entity, {0.5F, 0.5F, 0.5F}, transform, &error);
        REQUIRE_FALSE(id.is_null());
        return entity;
    }
};

// Records every delivery as "<event>:<self>-><other>" (payload entity bits).
struct ContactLog : bus::EventListener {
    std::vector<std::string> entries;

    void on_event(bus::Bus& /*bus*/, const bus::EventView& event) override {
        const base::Json* self = event.payload.find("self");
        const base::Json* other = event.payload.find("other");
        entries.push_back(std::string(event.event.view()) + ":" +
                          (self != nullptr ? std::to_string(self->as_int()) : "?") + "->" +
                          (other != nullptr ? std::to_string(other->as_int()) : "?"));
    }
};

[[nodiscard]] inline std::int64_t bits_of(ecs::EntityRef entity) {
    return static_cast<std::int64_t>(entity.to_bits());
}

} // namespace midday::physics::test
