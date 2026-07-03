// core/physics/physics_server.cpp — Jolt lives behind this TU (plumbing in
// core/physics/jolt_support.h) and nowhere else. The vendored engine runs
// under the repo FP contract with JPH_CROSS_PLATFORM_DETERMINISTIC
// (third_party/CMakeLists.txt, D-BUILD-073); this file adds the M0
// determinism discipline on top: single-threaded job system, fixed-dt
// steps, body-id-ordered sync, pair-id-sorted contact dispatch, and the
// thread-config lock.

#include "core/physics/physics_server.h"

#include "core/base/json.h"
#include "core/base/name.h"
#include "core/ecs/world.h"
#include "core/hierarchy/hierarchy.h"
#include "core/physics/jolt_support.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>

namespace midday::physics {
namespace {

using base::Error;
using base::Json;
using jolt::from_jolt;
using jolt::to_jolt;

[[nodiscard]] Error
entity_error(std::string_view code, std::string_view message, ecs::EntityRef entity) {
    Error error{std::string(code), std::string(message), Json::object()};
    error.details.set("entity_index", Json(entity.index));
    error.details.set("entity_generation", Json(entity.generation));
    return error;
}

[[nodiscard]] Error
thread_config_error(bool deterministic, std::uint32_t requested, std::uint32_t current) {
    Error error;
    if (deterministic) {
        error.code = "physics.config_locked";
        error.message = "deterministic mode pins the Jolt thread config; "
                        "divergent thread counts are refused, never clamped";
    } else {
        error.code = "physics.threads_unsupported";
        error.message = "M0 steps physics single-threaded in every mode; "
                        "m4-physics-full lifts play-mode threading";
    }
    error.details.set("mode", Json(deterministic ? "deterministic" : "play"));
    error.details.set("threads", Json(requested));
    error.details.set("required_threads", Json(1));
    error.details.set("current_threads", Json(current));
    return error;
}

} // namespace

// ---- the implementation -----------------------------------------------------

struct PhysicsServer::Impl {
    // Body-index -> binding (Jolt hands out dense indices deterministically;
    // ascending index IS ascending body id for live bodies).
    struct Binding {
        ecs::EntityRef entity;
        std::uint32_t id_bits = PhysicsBodyId::kInvalid;
        bool dynamic = false;

        [[nodiscard]] bool bound() const { return id_bits != PhysicsBodyId::kInvalid; }
    };

    // Field order follows the padding analyzer's optimal layout.
    JPH::JobSystemSingleThreaded job_system;
    JPH::PhysicsSystem system;
    ecs::World* world = nullptr;
    hierarchy::Hierarchy* hierarchy = nullptr;
    bus::Bus* bus = nullptr;
    jolt::BroadPhaseLayers broad_phase_layers;
    jolt::ObjectVsBroadPhase object_vs_broad_phase;
    jolt::ObjectLayerPairs object_layer_pairs;
    base::Name contact_began_name{"contact.began"};
    base::Name contact_ended_name{"contact.ended"};
    std::vector<Binding> bindings;
    JPH::TempAllocatorImpl temp_allocator;
    jolt::ContactCollector contacts;
    PhysicsStats stats;
    std::uint32_t bound_count = 0;
    PhysicsConfig config;

    Impl(ecs::World& world_in,
         hierarchy::Hierarchy& hierarchy_in,
         bus::Bus& bus_in,
         const PhysicsConfig& config_in)
        : job_system(JPH::cMaxPhysicsJobs), world(&world_in), hierarchy(&hierarchy_in),
          bus(&bus_in), temp_allocator(config_in.temp_allocator_bytes), config(config_in) {
        system.Init(config.max_bodies,
                    0, // body mutex count: Jolt default
                    config.max_body_pairs,
                    config.max_contact_constraints,
                    broad_phase_layers,
                    object_vs_broad_phase,
                    object_layer_pairs);
        system.SetGravity(to_jolt(config.gravity));
        system.SetContactListener(&contacts);
    }

    [[nodiscard]] const Binding* binding_of(PhysicsBodyId id) const {
        if (id.is_null())
            return nullptr;
        const std::uint32_t index = JPH::BodyID(id.value).GetIndex();
        if (index >= bindings.size())
            return nullptr;
        const Binding& binding = bindings[index];
        return binding.id_bits == id.value ? &binding : nullptr;
    }

    // Shared tail of both create paths: validate entity, create + add the
    // Jolt body, record the binding, write the PhysicsBody row.
    PhysicsBodyId bind_body(ecs::EntityRef entity,
                            const JPH::BodyCreationSettings& settings,
                            bool dynamic,
                            Error* error) {
        auto fail = [&](Error refusal) {
            if (error != nullptr)
                *error = std::move(refusal);
            return PhysicsBodyId{};
        };
        if (auto stale = world->check_alive(entity))
            return fail(std::move(*stale));
        if (!hierarchy->contains(entity))
            return fail(entity_error("physics.not_adopted",
                                     "physics bodies bind to hierarchy-adopted entities "
                                     "(the transform sync target)",
                                     entity));
        if (!body_of_entity(entity).is_null())
            return fail(entity_error(
                "physics.duplicate_body", "entity already has a physics body", entity));

        JPH::BodyInterface& bodies = system.GetBodyInterface();
        JPH::Body* body = bodies.CreateBody(settings);
        if (body == nullptr)
            return fail(Error{"physics.body_capacity",
                              "physics body pool exhausted (PhysicsConfig::max_bodies)",
                              Json::object().set("max_bodies", Json(config.max_bodies))});
        bodies.AddBody(body->GetID(),
                       dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);

        const PhysicsBodyId id{body->GetID().GetIndexAndSequenceNumber()};
        const std::uint32_t index = body->GetID().GetIndex();
        if (index >= bindings.size())
            bindings.resize(index + 1);
        bindings[index] = Binding{entity, id.value, dynamic};
        ++bound_count;
        ++stats.bodies_created;

        if (world->has<PhysicsBody>(entity)) {
            world->try_get<PhysicsBody>(entity)->id = id;
            (void)world->set_active<PhysicsBody>(entity, true);
        } else {
            (void)world->emplace<PhysicsBody>(entity, PhysicsBody{id});
        }
        return id;
    }

    [[nodiscard]] PhysicsBodyId body_of_entity(ecs::EntityRef entity) const {
        const PhysicsBody* body = world->try_get<PhysicsBody>(entity);
        return body != nullptr ? body->id : PhysicsBodyId{};
    }

    void unbind(Binding& binding) {
        JPH::BodyInterface& bodies = system.GetBodyInterface();
        const JPH::BodyID id{binding.id_bits};
        bodies.RemoveBody(id);
        bodies.DestroyBody(id);
        binding = Binding{};
        --bound_count;
    }

    // Phase-6 step 2 (header contract): reap dead-entity bodies and write
    // every live dynamic body's transform into its entity's LOCAL transform,
    // ascending body-id order. Bodies are hierarchy roots in M0, so local ==
    // world; the authored scale is preserved (physics has none).
    void sync_transforms() {
        JPH::BodyInterface& bodies = system.GetBodyInterface();
        for (Binding& binding : bindings) {
            if (!binding.bound())
                continue;
            if (!world->alive(binding.entity)) {
                unbind(binding);
                ++stats.bodies_reaped;
                continue;
            }
            if (!binding.dynamic)
                continue;
            JPH::RVec3 position;
            JPH::Quat rotation;
            bodies.GetPositionAndRotation(JPH::BodyID(binding.id_bits), position, rotation);
            const math::Transform* current = hierarchy->local_of(binding.entity);
            math::Transform local;
            local.translation = from_jolt(position);
            local.rotation = from_jolt(rotation);
            if (current != nullptr)
                local.scale = current->scale;
            (void)hierarchy->set_local(binding.entity, local);
            ++stats.sync_writes;
        }
    }

    // Phase-6 step 3: began (sorted by pair id), then ended (same sort);
    // per pair, the lower body id's entity hears first. Cause: the phase-6
    // marker — the bus journals every trigger before dispatch.
    void dispatch_contacts(std::uint64_t phase_record_id) {
        jolt::sort_unique(contacts.began);
        jolt::sort_unique(contacts.ended);
        for (const jolt::PendingContact& contact : contacts.began) {
            ++stats.contacts_began;
            trigger_pair(contact, contact_began_name, true, phase_record_id);
        }
        for (const jolt::PendingContact& contact : contacts.ended) {
            ++stats.contacts_ended;
            trigger_pair(contact, contact_ended_name, false, phase_record_id);
        }
        contacts.began.clear();
        contacts.ended.clear();
    }

    void trigger_pair(const jolt::PendingContact& contact,
                      base::Name event,
                      bool began,
                      std::uint64_t cause_id) {
        const Binding* first = binding_of(PhysicsBodyId{contact.a});
        const Binding* second = binding_of(PhysicsBodyId{contact.b});
        if (first == nullptr || second == nullptr) {
            ++stats.contacts_unbound; // destroyed mid-window: nothing to address
            return;
        }
        trigger_one(first->entity, second->entity, contact, began, false, event, cause_id);
        trigger_one(second->entity, first->entity, contact, began, true, event, cause_id);
    }

    void trigger_one(ecs::EntityRef self,
                     ecs::EntityRef other,
                     const jolt::PendingContact& contact,
                     bool began,
                     bool flip_normal,
                     base::Name event,
                     std::uint64_t cause_id) {
        Json payload = Json::object();
        payload.set("self", Json(static_cast<std::int64_t>(self.to_bits())));
        payload.set("other", Json(static_cast<std::int64_t>(other.to_bits())));
        if (began) {
            payload.set("position", jolt::vec3_json(contact.position));
            payload.set("normal",
                        jolt::vec3_json(flip_normal ? -contact.normal_a_b : contact.normal_a_b));
            // M0: Jolt knows impulses only after the solver; m4-physics-full
            // wires real contact impulses (D-BUILD-075).
            payload.set("impulse", Json(0.0));
        }
        const bus::TriggerResult result =
            bus->trigger(bus::EventKey::entity(self), event, payload, cause_id);
        if (result.error.has_value())
            ++stats.trigger_refusals; // the bus journaled the refusal
        else
            ++stats.contact_triggers;
    }
};

// ---- public surface -----------------------------------------------------------

PhysicsServerCreateResult PhysicsServer::create(ecs::World& world,
                                                hierarchy::Hierarchy& hierarchy,
                                                bus::Bus& bus,
                                                PhysicsConfig config) {
    if (config.threads != 1)
        return {nullptr, thread_config_error(config.deterministic, config.threads, 1)};
    jolt::ensure_runtime();

    reflect::ClassDesc desc;
    desc.name = base::Name("PhysicsBody");
    desc.doc = "Opaque physics body handle bridging this entity to the physics server "
               "(written only by physics::PhysicsServer).";
    world.register_component<PhysicsBody>(std::move(desc));

    auto impl = std::make_unique<Impl>(world, hierarchy, bus, config);
    PhysicsServerCreateResult result;
    result.server.reset(new PhysicsServer(std::move(impl))); // private ctor: no make_unique
    return result;
}

PhysicsServer::PhysicsServer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

PhysicsServer::~PhysicsServer() = default;

std::optional<Error> PhysicsServer::reconfigure_threads(std::uint32_t threads) {
    if (threads == impl_->config.threads)
        return std::nullopt; // same config: nothing to lock against
    return thread_config_error(impl_->config.deterministic, threads, impl_->config.threads);
}

const PhysicsConfig& PhysicsServer::config() const {
    return impl_->config;
}

void PhysicsServer::set_gravity(math::Vec3 gravity) {
    impl_->config.gravity = gravity;
    impl_->system.SetGravity(to_jolt(gravity));
}

math::Vec3 PhysicsServer::gravity() const {
    return from_jolt(impl_->system.GetGravity());
}

PhysicsBodyId PhysicsServer::create_dynamic_box(ecs::EntityRef entity,
                                                math::Vec3 half_extents,
                                                const math::Transform& world_transform,
                                                base::Error* error) {
    // Jolt's convex radius must not exceed the smallest half extent.
    const float min_extent = std::min({half_extents.x, half_extents.y, half_extents.z});
    if (!(min_extent > 0.0F)) {
        if (error != nullptr)
            *error = entity_error(
                "physics.bad_shape", "box half extents must be strictly positive", entity);
        return {};
    }
    const float convex_radius = std::min(JPH::cDefaultConvexRadius, 0.5F * min_extent);
    JPH::BodyCreationSettings settings(new JPH::BoxShape(to_jolt(half_extents), convex_radius),
                                       to_jolt(world_transform.translation),
                                       to_jolt(world_transform.rotation),
                                       JPH::EMotionType::Dynamic,
                                       jolt::kLayerMoving);
    const PhysicsBodyId id = impl_->bind_body(entity, settings, true, error);
    if (!id.is_null()) {
        // Bodies are roots: local == world from the first byte.
        (void)impl_->hierarchy->set_local(entity, world_transform);
    }
    return id;
}

PhysicsBodyId
PhysicsServer::create_ground_plane(ecs::EntityRef entity, float y, base::Error* error) {
    JPH::BodyCreationSettings settings(new JPH::PlaneShape(JPH::Plane::sFromPointAndNormal(
                                           JPH::Vec3(0.0F, y, 0.0F), JPH::Vec3::sAxisY())),
                                       JPH::RVec3::sZero(),
                                       JPH::Quat::sIdentity(),
                                       JPH::EMotionType::Static,
                                       jolt::kLayerStatic);
    const PhysicsBodyId id = impl_->bind_body(entity, settings, false, error);
    if (!id.is_null()) {
        math::Transform local;
        local.translation = {0.0F, y, 0.0F};
        (void)impl_->hierarchy->set_local(entity, local);
    }
    return id;
}

std::optional<Error> PhysicsServer::destroy_body(ecs::EntityRef entity) {
    if (auto stale = impl_->world->check_alive(entity))
        return stale;
    const PhysicsBodyId id = impl_->body_of_entity(entity);
    const Impl::Binding* binding = impl_->binding_of(id);
    if (binding == nullptr)
        return entity_error("physics.no_body", "entity has no physics body", entity);
    impl_->unbind(impl_->bindings[JPH::BodyID(id.value).GetIndex()]);
    ++impl_->stats.bodies_destroyed;
    impl_->world->try_get<PhysicsBody>(entity)->id = PhysicsBodyId{};
    (void)impl_->world->set_active<PhysicsBody>(entity, false);
    return std::nullopt;
}

PhysicsBodyId PhysicsServer::body_of(ecs::EntityRef entity) const {
    if (!impl_->world->alive(entity))
        return {};
    return impl_->body_of_entity(entity);
}

ecs::EntityRef PhysicsServer::entity_of(PhysicsBodyId id) const {
    const Impl::Binding* binding = impl_->binding_of(id);
    return binding != nullptr ? binding->entity : ecs::EntityRef{};
}

std::uint32_t PhysicsServer::body_count() const {
    return impl_->bound_count;
}

std::optional<math::Transform> PhysicsServer::body_transform(PhysicsBodyId id) const {
    if (impl_->binding_of(id) == nullptr)
        return std::nullopt;
    JPH::RVec3 position;
    JPH::Quat rotation;
    impl_->system.GetBodyInterface().GetPositionAndRotation(
        JPH::BodyID(id.value), position, rotation);
    math::Transform transform;
    transform.translation = from_jolt(position);
    transform.rotation = from_jolt(rotation);
    return transform;
}

const PhysicsStats& PhysicsServer::stats() const {
    return impl_->stats;
}

void PhysicsServer::on_phase(tick::TickLoop& /*loop*/, const tick::PhaseContext& context) {
    if (context.phase != tick::Phase::kPhysics)
        return; // registered on phase 6 only; belt and braces
    const JPH::EPhysicsUpdateError update_error =
        impl_->system.Update(static_cast<float>(context.dt),
                             static_cast<int>(impl_->config.collision_steps),
                             &impl_->temp_allocator,
                             &impl_->job_system);
    ++impl_->stats.steps;
    if (update_error != JPH::EPhysicsUpdateError::None)
        ++impl_->stats.step_errors; // capacity overflow; counted, never thrown
    impl_->sync_transforms();
    impl_->dispatch_contacts(context.phase_record_id);
}

} // namespace midday::physics
