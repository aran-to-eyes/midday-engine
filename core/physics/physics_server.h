// core/physics/physics_server.h — the minimal deterministic physics server
// over JoltPhysics (m0-jolt-minimal; spec section 6). M0 surface: dynamic
// boxes + a static ground plane, a fixed-dt step wired into tick phase 6,
// transform sync back into the hierarchy, and contact events on the bus.
// m4-physics-full expands this to the whole rigid-body surface behind the
// same server interface — nothing here leaks a Jolt type.
//
// DETERMINISTIC CONFIG LOCK (spec 6: "replay-mode threading config is pinned
// and documented"). The server is constructed with its thread config; in
// deterministic mode (the default, and the ONLY mode the determinism lanes
// accept) the config is LOCKED to threads == 1: constructing or
// reconfiguring with a divergent thread count is the structured refusal
// "physics.config_locked" — never a silent clamp. Jolt is deterministic
// given the same build, inputs, and a fixed thread/step configuration; the
// lock is what makes that premise unbreakable from the outside. M0 steps
// through Jolt's single-threaded job system in EVERY mode; a non-
// deterministic multi-thread request is "physics.threads_unsupported" until
// m4 lifts it (a scope limit, deliberately distinct from the lock).
//
// PHASE-6 STEP ORDER (on_phase, registered on tick::Phase::kPhysics):
//   1. Jolt steps once with the loop's fixed dt (collision_steps sub-steps).
//   2. Transform sync — physics writes are sim writes: every dynamic body's
//      position/rotation is written into its entity's hierarchy LOCAL
//      transform (M0 bodies are hierarchy roots; authored scale preserved),
//      in ascending body-id order, BEFORE structural apply (phase 8
//      propagates the world matrices). Bodies whose entity died are reaped
//      here, in the same deterministic order.
//   3. Contact dispatch — contacts collected DURING the step trigger AFTER
//      it, on the bus: all contact.began (sorted by body-pair id), then all
//      contact.ended (same sort). Per pair (a, b), a < b by body id: first
//      the trigger on a's entity channel {self: a, other: b}, then on b's.
//      Every trigger journals (the bus does it) with cause =
//      PhaseContext::phase_record_id — the phase-6 marker.
//   Listeners therefore always observe post-step transforms. M0 payload
//   note: `impulse` is 0.0 — Jolt knows impulses only after the solver;
//   m4-physics-full wires real contact impulses (D-BUILD-075).
//
// No exceptions anywhere in the step path; body creation returns structured
// errors ("physics.not_adopted", "physics.duplicate_body",
// "physics.body_capacity", ...). Bus refusals during dispatch are counted
// (the bus already journaled them); one bad trigger never stops the step.

#pragma once

#include "core/base/error.h"
#include "core/bus/bus.h"
#include "core/ecs/entity.h"
#include "core/math/vec.h"
#include "core/math/xform.h"
#include "core/tick/tick_loop.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace midday::ecs {
class World;
}

namespace midday::hierarchy {
class Hierarchy;
}

namespace midday::physics {

// Opaque body handle: Jolt's body id bits (index + sequence number). Body
// ids are a pure function of the create/destroy sequence — they ARE the
// deterministic sort key for sync order and contact-pair order.
struct PhysicsBodyId {
    static constexpr std::uint32_t kInvalid = 0xffffffffU;

    std::uint32_t value = kInvalid;

    [[nodiscard]] bool is_null() const { return value == kInvalid; }

    friend bool operator==(const PhysicsBodyId&, const PhysicsBodyId&) = default;
};

// The ECS bridge: entities carry their body handle as an ordinary component
// (registered by the server — one PhysicsServer per World). The row persists
// for the entity lifetime like every component; destroy_body nulls the id
// and deactivates the row (there is deliberately no component-remove API,
// D-BUILD-026).
struct PhysicsBody {
    PhysicsBodyId id;
};

struct PhysicsConfig {
    // Deterministic mode (default, and what every determinism lane runs):
    // the thread config below is LOCKED — divergence refuses construction.
    bool deterministic = true;
    // M0: exactly 1 (single-threaded island solve). Part of the replay
    // identity; m4 widens play-mode.
    std::uint32_t threads = 1;
    // Jolt collision sub-steps per fixed tick.
    std::uint32_t collision_steps = 1;
    math::Vec3 gravity{0.0F, -9.81F, 0.0F};
    // Capacities (Jolt pre-allocates; exceeding max_bodies refuses creation
    // with "physics.body_capacity").
    std::uint32_t max_bodies = 1024;
    std::uint32_t max_body_pairs = 1024;
    std::uint32_t max_contact_constraints = 1024;
    // Per-step scratch arena for Jolt (bytes).
    std::uint32_t temp_allocator_bytes = 10U * 1024U * 1024U;
};

struct PhysicsStats {
    std::uint64_t steps = 0;       // phase-6 Jolt updates run
    std::uint64_t step_errors = 0; // Jolt update errors (capacity overflow)
    std::uint64_t bodies_created = 0;
    std::uint64_t bodies_destroyed = 0; // explicit destroy_body
    std::uint64_t bodies_reaped = 0;    // entity died; body reaped at sync
    std::uint64_t sync_writes = 0;      // hierarchy set_local writes
    std::uint64_t contacts_began = 0;   // pairs collected during steps
    std::uint64_t contacts_ended = 0;
    std::uint64_t contact_triggers = 0; // bus triggers accepted
    std::uint64_t trigger_refusals = 0; // bus refusals (journaled by the bus)
    std::uint64_t contacts_unbound = 0; // pair skipped: body no longer bound
};

class PhysicsServer;

struct PhysicsServerCreateResult {
    std::unique_ptr<PhysicsServer> server; // null on refusal
    std::optional<base::Error> error;      // physics.config_locked | physics.threads_unsupported
};

class PhysicsServer final : public tick::PhaseHook {
public:
    // Validates the config (the deterministic lock above), boots Jolt, and
    // registers the PhysicsBody component — one server per World, at BOOT
    // (a second registration aborts loudly, the reflect D-BUILD-023
    // precedent). All three collaborators must outlive the server. Wire the
    // step with loop.add_hook(tick::Phase::kPhysics, *server).
    static PhysicsServerCreateResult create(ecs::World& world,
                                            hierarchy::Hierarchy& hierarchy,
                                            bus::Bus& bus,
                                            PhysicsConfig config = {});

    ~PhysicsServer(); // PhaseHook's dtor is protected/non-virtual by design

    PhysicsServer(const PhysicsServer&) = delete;
    PhysicsServer& operator=(const PhysicsServer&) = delete;
    PhysicsServer(PhysicsServer&&) = delete;
    PhysicsServer& operator=(PhysicsServer&&) = delete;

    // ---- config (the lock surface) -----------------------------------------
    // Deterministic mode refuses ANY divergent thread count
    // ("physics.config_locked"); non-deterministic mode refuses != 1 in M0
    // ("physics.threads_unsupported"). Same-count reconfigure is a no-op.
    std::optional<base::Error> reconfigure_threads(std::uint32_t threads);

    [[nodiscard]] const PhysicsConfig& config() const;

    // Gravity is an ordinary sim parameter, NOT part of the config lock:
    // setting it is a deterministic sim write (part of the operation
    // script), effective from the next step.
    void set_gravity(math::Vec3 gravity);

    [[nodiscard]] math::Vec3 gravity() const;

    // ---- bodies (M0: boxes + ground plane; the shape zoo is m4) ------------
    // Creates a dynamic box bound to `entity` (which must be alive and
    // hierarchy-adopted — the sync target). The entity's local transform is
    // set to `world_transform` immediately (bodies are roots: local ==
    // world); physics ignores its scale and the sync preserves it.
    PhysicsBodyId create_dynamic_box(ecs::EntityRef entity,
                                     math::Vec3 half_extents,
                                     const math::Transform& world_transform,
                                     base::Error* error = nullptr);

    // Creates the static ground: an upward-facing Jolt plane at world
    // height y, bound to `entity` (same adoption contract).
    PhysicsBodyId create_ground_plane(ecs::EntityRef entity, float y, base::Error* error = nullptr);

    // Removes and destroys the entity's body; the PhysicsBody row is nulled
    // and deactivated. "physics.no_body" when none is bound.
    std::optional<base::Error> destroy_body(ecs::EntityRef entity);

    // ---- introspection ------------------------------------------------------
    [[nodiscard]] PhysicsBodyId body_of(ecs::EntityRef entity) const;
    [[nodiscard]] ecs::EntityRef entity_of(PhysicsBodyId id) const;
    [[nodiscard]] std::uint32_t body_count() const;

    // The body's current world-space transform (scale 1); nullopt for an
    // unbound id. Post-step this equals the entity's synced local transform.
    [[nodiscard]] std::optional<math::Transform> body_transform(PhysicsBodyId id) const;

    [[nodiscard]] const PhysicsStats& stats() const;

    // ---- the phase-6 body (documented order above) --------------------------
    void on_phase(tick::TickLoop& loop, const tick::PhaseContext& context) override;

private:
    struct Impl; // physics_server.cpp — the only place Jolt types exist

    explicit PhysicsServer(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace midday::physics
