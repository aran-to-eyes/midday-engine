// core/loader/spawn.cpp — SceneFile -> live World: the boot-phase
// instantiation path every authored scene takes (spec section 7: no
// code-assembled entities in public paths — THIS is the public path).
// Document order everywhere; one structural flush realizes children under
// states; every spawn journals a FLIGHT "scene.spawn" record citing the
// caller's cause (the run verb's run.config root record), so a bundle
// explains its own boot.

#include "core/ecs/world.h"
#include "core/hierarchy/hierarchy.h"
#include "core/journal/writer.h"
#include "core/loader/loader.h"
#include "core/loader/prefab_instantiate.h"
#include "core/physics/physics_server.h"

#include <string>
#include <utility>
#include <vector>

namespace midday::loader {

namespace {

std::string entity_form(ecs::EntityRef ref) {
    return "entity:" + std::to_string(ref.index) + "#" + std::to_string(ref.generation);
}

base::Error internal_error(std::string what, const base::Error& cause) {
    base::Error error;
    error.code = "loader.spawn";
    error.message = std::move(what) + ": [" + cause.code + "] " + cause.message;
    error.details.set("cause", cause.to_json());
    return error;
}

} // namespace

bool scene_uses_physics(const SceneFile& scene) {
    for (const SceneEntityDesc& entity : scene.entities)
        if (entity.components.collider.has_value())
            return true;
    return false;
}

std::optional<base::Error> register_scene_events(const SceneFile& scene,
                                                 reflect::Registry& registry) {
    for (const EventDecl& decl : scene.events.events) {
        const base::Name name{decl.name};
        if (registry.find_event(name) != nullptr) {
            base::Error error;
            error.code = "loader.duplicate";
            error.message = "event '" + decl.name + "' is already registered";
            error.details.set("event", decl.name);
            return error;
        }
        reflect::EventDesc desc;
        desc.name = name;
        desc.doc = decl.doc;
        for (const EventFieldDecl& field : decl.payload)
            desc.payload.push_back(reflect::EventFieldDesc{base::Name(field.name), field.type, ""});
        registry.add_event(std::move(desc));
    }
    return std::nullopt;
}

bus::EventKey resolve_key(std::string_view spelling, ecs::EntityRef host) {
    if (spelling == "self" || spelling == "root")
        return bus::EventKey::entity(host);
    if (spelling == "global")
        return bus::EventKey::named(base::Name("global"));
    return bus::EventKey::named(base::Name(spelling));
}

SpawnResult spawn_scene(const SceneFile& scene,
                        ecs::World& world,
                        hierarchy::Hierarchy& hierarchy,
                        statechart::Statechart& chart,
                        physics::PhysicsServer* physics,
                        journal::Writer& journal,
                        std::uint64_t cause_id) {
    SpawnResult result;

    // The authored-name marker component (loader.h contract: one scene per
    // World in M0 — a second spawn_scene into the same World aborts on the
    // re-registration, exactly like any boot-path misuse).
    reflect::ClassDesc marker;
    marker.name = base::Name("SceneEntity");
    marker.doc = "loader marker: the authored scene/machine-child entity name";
    marker.properties.push_back(
        reflect::PropertyDesc{base::Name("name"),
                              reflect::TypeDesc::scalar(reflect::TypeKind::kName),
                              base::Json(),
                              0,
                              ""});
    world.register_component<SceneEntity>(std::move(marker));

    struct PendingChild {
        ecs::EntityRef ref;
        math::Transform at;
    };

    std::vector<PendingChild> pending_children;
    const auto fail = [&result](base::Error error) {
        result.error = std::move(error);
        return std::move(result);
    };

    for (const SceneEntityDesc& desc : scene.entities) {
        // Prefab entities (m1-scene-format's `prefab:` + `at:` + `override:`
        // grammar): the resolved *.entity.yaml materializes onto a directly-
        // spawned root (this node's boot-path precedent, unchanged for
        // inline entities below) via core/loader/prefab_instantiate.h — the
        // SAME override-application + machine-instantiate engine the
        // runtime's world.spawn(prefab) rides (core/loader/prefab_spawn.h).
        // A missing prefab FILE is a lenient-only Gap in `scene print`
        // (core/loader/scene_prefab.cpp); spawn_scene has no lenient mode,
        // so an unresolved reference here is a hard boot-time refusal.
        if (desc.kind == SceneEntityKind::kPrefab) {
            // kPrefab always carries a prefab descriptor, but the explicit guard
            // is what bugprone-unchecked-optional-access needs: it cannot
            // correlate the `kind` discriminant with the sibling optional, and
            // libstdc++'s model flags the deref where libc++'s does not — so
            // verify-linux caught what the macOS local gate's libc++ tidy missed.
            if (!desc.prefab.has_value()) {
                base::Error broken;
                broken.code = "loader.bad_ref";
                broken.message = "entity '" + std::string(desc.name.view()) +
                                 "': prefab kind carries no prefab descriptor (loader invariant)";
                return fail(std::move(broken));
            }
            const PrefabInstanceDesc& prefab = *desc.prefab;
            if (!prefab.resolved) {
                base::Error missing;
                missing.code = "loader.bad_ref";
                missing.message = "entity '" + std::string(desc.name.view()) + "': prefab '" +
                                  prefab.prefab_ref.path_authored + "' did not resolve";
                return fail(std::move(missing));
            }
            const EntityFile& entity_file = scene.entity_prefabs.at(prefab.entity_index);

            base::Error prefab_spawn_error;
            const ecs::EntityRef entity = world.spawn(&prefab_spawn_error);
            if (entity.is_null())
                return fail(internal_error("spawn refused", prefab_spawn_error));
            if (auto error = hierarchy.adopt(entity))
                return fail(internal_error("adopt refused", *error));
            if (auto error = world.emplace<SceneEntity>(entity, SceneEntity{desc.name}))
                return fail(internal_error("marker refused", *error));
            if (auto error = hierarchy.set_local(entity, prefab.at))
                return fail(internal_error("transform refused", *error));

            ResolvedMachinesResult resolved =
                resolve_prefab_machines(entity_file, prefab.overrides, entity_file.path);
            if (resolved.error.has_value())
                return fail(internal_error("prefab override refused", *resolved.error));

            MaterializeResult materialized = materialize_prefab(world,
                                                                hierarchy,
                                                                chart,
                                                                journal,
                                                                /*tick=*/0,
                                                                resolved.machines,
                                                                entity,
                                                                prefab.prefab_ref.path_resolved,
                                                                cause_id);
            if (materialized.error.has_value())
                return fail(internal_error("prefab instantiate refused", *materialized.error));

            result.stats.entities += 1;
            for (const MaterializedMachine& seat : materialized.machines) {
                result.machines.push_back(MachineSeat{
                    .id = seat.id, .machine = seat.machine, .entity = desc.name, .host = entity});
                result.stats.machines += 1;
                for (const StateScriptRef& script : seat.scripts)
                    result.scripts.push_back(ScriptSeat{.machine = seat.id,
                                                        .region = script.region,
                                                        .state = script.state,
                                                        .ref = script.ref,
                                                        .path = script.path});
            }
            for (const PrefabChild& child : materialized.children) {
                pending_children.push_back(PendingChild{child.ref, child.at});
                result.stats.state_children += 1;
            }
            continue;
        }
        base::Error spawn_error;
        const ecs::EntityRef entity = world.spawn(&spawn_error);
        if (entity.is_null())
            return fail(internal_error("spawn refused", spawn_error));
        if (auto error = hierarchy.adopt(entity))
            return fail(internal_error("adopt refused", *error));
        if (auto error = world.emplace<SceneEntity>(entity, SceneEntity{desc.name}))
            return fail(internal_error("marker refused", *error));
        if (desc.components.transform.has_value())
            if (auto error = hierarchy.set_local(entity, *desc.components.transform))
                return fail(internal_error("transform refused", *error));

        base::Json payload = base::Json::object();
        payload.set("name", desc.name.view());
        payload.set("entity", entity_form(entity));
        journal.record(0, journal::Tier::Flight, "scene.spawn", cause_id, std::move(payload));
        result.stats.entities += 1;

        if (desc.components.collider.has_value()) {
            if (physics == nullptr) {
                base::Error missing;
                missing.code = "loader.spawn";
                missing.message = "scene references physics but no server was built";
                return fail(
                    internal_error("entity '" + std::string(desc.name.view()) + "'", missing));
            }
            const math::Transform placed =
                desc.components.transform.value_or(math::Transform::identity());
            base::Error body_error;
            physics::PhysicsBodyId body;
            if (desc.components.collider->box) {
                const math::Vec3 half{desc.components.collider->size.x * 0.5F,
                                      desc.components.collider->size.y * 0.5F,
                                      desc.components.collider->size.z * 0.5F};
                body = physics->create_dynamic_box(entity, half, placed, &body_error);
            } else {
                body = physics->create_ground_plane(entity, placed.translation.y, &body_error);
            }
            if (body.is_null())
                return fail(internal_error("physics body refused", body_error));
            result.stats.bodies += 1;
        }

        for (const std::uint32_t machine_index : desc.machines) {
            const MachineFile& machine = scene.machines[machine_index];
            const statechart::InstantiateResult instantiated =
                chart.instantiate(machine.desc, entity, cause_id);
            if (instantiated.error.has_value())
                return fail(internal_error("machine '" + std::string(machine.desc.name.view()) +
                                               "' refused",
                                           *instantiated.error));
            result.machines.push_back(MachineSeat{.id = instantiated.machine,
                                                  .machine = machine.desc.name,
                                                  .entity = desc.name,
                                                  .host = entity});
            result.stats.machines += 1;

            for (const StateScriptRef& script : machine.scripts)
                result.scripts.push_back(ScriptSeat{.machine = instantiated.machine,
                                                    .region = script.region,
                                                    .state = script.state,
                                                    .ref = script.ref,
                                                    .path = script.path});

            for (const StateChildren& children : machine.children) {
                const ecs::EntityRef state_entity =
                    chart.state_entity(instantiated.machine, children.region, children.state);
                if (state_entity.is_null()) {
                    base::Error missing;
                    missing.code = "loader.spawn";
                    missing.message =
                        "state entity missing for '" + std::string(children.state.view()) + "'";
                    return fail(std::move(missing));
                }
                for (const StateChildDesc& child : children.children) {
                    base::Error child_error;
                    const ecs::EntityRef child_ref = world.spawn(&child_error);
                    if (child_ref.is_null())
                        return fail(internal_error("child spawn refused", child_error));
                    if (auto error =
                            world.emplace<SceneEntity>(child_ref, SceneEntity{child.entity}))
                        return fail(internal_error("child marker refused", *error));
                    if (auto error = hierarchy.queue_attach(child_ref, state_entity))
                        return fail(internal_error("child attach refused", *error));
                    base::Json child_payload = base::Json::object();
                    child_payload.set("name", child.entity.view());
                    child_payload.set("entity", entity_form(child_ref));
                    child_payload.set("under_state", children.state.view());
                    journal.record(0,
                                   journal::Tier::Flight,
                                   "scene.spawn",
                                   cause_id,
                                   std::move(child_payload));
                    pending_children.push_back(PendingChild{child_ref, child.at});
                    result.stats.state_children += 1;
                }
            }
        }
    }

    // ONE structural flush realizes the queued child attachments (the
    // A.1 phase-8 semantics, run at boot), then local transforms land and
    // the world matrices propagate so tick 1 starts from settled state.
    if (auto error = world.flush_structural())
        return fail(internal_error("structural flush refused", *error));
    for (const PendingChild& child : pending_children)
        if (auto error = hierarchy.set_local(child.ref, child.at))
            return fail(internal_error("child transform refused", *error));
    hierarchy.propagate();
    return result;
}

} // namespace midday::loader
