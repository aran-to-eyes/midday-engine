// core/loader/component_materialize.h — the ONE component-materialization
// dispatcher (M2 0B, #12b): every authored component entry a spawn path
// meets — a scene entity's non-native inline components, a prefab's `base:`
// list, a machine state's `components:` list, a state child's components —
// routes through here, exactly once. Native names (Transform / Collider /
// RigidBody) go to the factored native materializers; every other name goes
// to the injected ScriptComponentMaterializer (ts/runtime's
// ComponentInstanceHost in production — core/ never depends on ts/, the
// StateHooks precedent). A component NOTHING claims refuses with a
// structured error: nothing is ever silently omitted.
//
// Native coverage is deliberately narrow in 0B: a generic `Transform: {at:
// [x,y,z]}` materializes (hierarchy set_local + a JS-side mirror when a
// script materializer is present, so `entity.get(Transform)` reads the
// placed value); generic Collider/RigidBody entries REFUSE
// ("component.native_unsupported") until the physics node wires body
// creation into the generic path — the scene's typed inline `components:`
// path (loader.h ComponentSet, spawn.cpp) keeps full native coverage,
// unchanged. A state child's Transform also refuses: `at:` owns child
// placement.
//
// ORDER IS THE CONTRACT (D1): materialization order = bus subscription
// order = dispatch order. Base components materialize in authored order
// BEFORE machines instantiate; state components materialize per machine in
// document order (region by region, states document order, entries authored
// order) — that order IS the attach order the statechart's enter-2/exit-3
// slots replay.

#pragma once

#include "core/base/error.h"
#include "core/base/name.h"
#include "core/ecs/entity.h"
#include "core/loader/entity_format.h"
#include "core/math/xform.h"
#include "core/statechart/statechart.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace midday::hierarchy {
class Hierarchy;
}

namespace midday::loader {

// The script-side seam. ts/runtime's ComponentInstanceHost implements it;
// tests inject doubles. All three calls journal-and-effect on the far side;
// a refusal aborts the spawn (first failure wins, the spawn_scene rule).
class ScriptComponentMaterializer {
public:
    ScriptComponentMaterializer() = default;
    ScriptComponentMaterializer(const ScriptComponentMaterializer&) = default;
    ScriptComponentMaterializer& operator=(const ScriptComponentMaterializer&) = default;
    ScriptComponentMaterializer(ScriptComponentMaterializer&&) = default;
    ScriptComponentMaterializer& operator=(ScriptComponentMaterializer&&) = default;

    // An entity-lifetime component on `entity` (a `base:` entry, a scene
    // entity's inline non-native component, a state child's component).
    virtual std::optional<base::Error> materialize_base(ecs::EntityRef entity,
                                                        const GenericComponentEntry& entry,
                                                        std::uint64_t cause_id) = 0;

    // A state-owned component (spec 4.1): instance lifetime = entity
    // lifetime, ACTIVITY = the state's — the implementation registers
    // statechart::ComponentHooks on (machine, region, state) so the A.2.1
    // enter-2/exit-3 slots drive it.
    virtual std::optional<base::Error> materialize_state(statechart::Statechart& chart,
                                                         statechart::MachineId machine,
                                                         base::Name region,
                                                         base::Name state,
                                                         ecs::EntityRef host,
                                                         const statechart::StateComponentDesc& desc,
                                                         std::uint64_t cause_id) = 0;

    // The JS-side Transform mirror for a natively-placed Transform — a
    // `base:` generic entry, a scene entity's inline `Transform:`, or
    // prefab/spawn `at:` placement (mirror_seeded_transform below owns the
    // policy for the latter two): `entity.get(Transform)` must read the
    // authored placement. A MATERIALIZATION-TIME SNAPSHOT in 0B (live
    // native<->JS transform sync is a later node's seam).
    virtual std::optional<base::Error> mirror_native_transform(ecs::EntityRef entity,
                                                               const math::Transform& value,
                                                               std::uint64_t cause_id) = 0;

protected:
    ~ScriptComponentMaterializer() = default; // never deleted through this seam
};

// The despawn-lifecycle seam (M2 0B track D, FUSED-SPEC D4): the three
// script-host calls the phase-8 despawn path makes, in this exact temporal
// split — despawn_exit runs at PREPARE (pre-flush, the entity provably
// alive, right after the statechart exit chains covered its STATE seats);
// note_despawn + reap_entity run at REALIZE (post-flush, the entity
// provably dead). ts/runtime's ComponentInstanceHost implements it (its
// note_despawn forwards to the primitives seat); tests inject doubles.
// None of the three may fail structurally: script exceptions land in the
// host's own first_error(), never unwind phase 8 — the ComponentHooks rule.
class DespawnHooks {
public:
    DespawnHooks() = default;
    DespawnHooks(const DespawnHooks&) = default;
    DespawnHooks& operator=(const DespawnHooks&) = default;
    DespawnHooks(DespawnHooks&&) = default;
    DespawnHooks& operator=(DespawnHooks&&) = default;

    // PREPARE, pre-flush: invoke onExit on `ref`'s BASE seats in REVERSE
    // attach order (state seats already exited through the chart's exit-3
    // slot). `cause_id` is the despawn's own journal record.
    virtual void despawn_exit(ecs::EntityRef ref, std::uint64_t cause_id) = 0;

    // REALIZE, post-flush: record the actual REAP tick (the 0A G2 closure —
    // a later stale-ref query names this tick, never null).
    virtual void note_despawn(ecs::EntityRef ref, std::uint64_t reap_tick) = 0;

    // REALIZE, post-flush, after note_despawn: release `ref`'s seats (state
    // first, then base, reverse attach order). Invokes NO hooks; idempotent.
    virtual void reap_entity(ecs::EntityRef ref) = 0;

protected:
    ~DespawnHooks() = default; // never deleted through this seam
};

// How a spawn path materializes (threaded through spawn_scene /
// materialize_prefab; every pre-0B caller's default is scripts = nullptr +
// eager entry — byte-identical behavior for component-free content).
// defer_initial_entry REQUIRES the caller to run
// loader::start_initial_entries (loader.h) after seating state scripts;
// state components REQUIRE both fields (a late synthetic onEnter is
// unacceptable — the D2 split).
struct SpawnOptions {
    ScriptComponentMaterializer* scripts = nullptr;
    bool defer_initial_entry = false;
};

// One generic component entry onto `entity`. `allow_native_transform`
// distinguishes a prefab root's `base:` list (true) from a state child's
// components (false — `at:` owns child placement).
std::optional<base::Error> materialize_base_component(hierarchy::Hierarchy& hierarchy,
                                                      const SpawnOptions& options,
                                                      ecs::EntityRef entity,
                                                      const GenericComponentEntry& entry,
                                                      std::uint64_t cause_id,
                                                      bool allow_native_transform);

// A whole `base:`-shaped list, authored order (= subscription order).
std::optional<base::Error>
materialize_entity_base(hierarchy::Hierarchy& hierarchy,
                        const SpawnOptions& options,
                        ecs::EntityRef entity,
                        const std::vector<GenericComponentEntry>& components,
                        std::uint64_t cause_id,
                        bool allow_native_transform);

// Every state component of an ALREADY-instantiated (deferred) machine, in
// document order. Refuses "component.requires_deferred_entry" /
// "component.no_materializer" up front when the options cannot honor the
// seating contract; native names refuse "component.native_unsupported".
std::optional<base::Error> materialize_machine_components(statechart::Statechart& chart,
                                                          const SpawnOptions& options,
                                                          statechart::MachineId machine,
                                                          const statechart::MachineDesc& desc,
                                                          ecs::EntityRef host,
                                                          std::uint64_t cause_id);

// True when any state (at any depth) of the machine owns components — the
// same scan materialize_machine_components gates on, exposed for the
// transform-mirror policy below.
[[nodiscard]] bool machine_has_state_components(const statechart::MachineDesc& desc);

// M2 0B council fix G6: the `entity.get(Transform)` mirror must cover EVERY
// path that seats a native Transform on an entity hosting script components
// — scene inline `Transform:` and prefab/spawn `at:` placement, not only
// the generic `Transform: {at:}` entry. Mirrors `value` into the script
// directory (a materialization-time snapshot, the mirror_native_transform
// contract) when ALL of:
//   * a script materializer is wired (options.scripts),
//   * the entity actually hosts script components (`components` carries a
//     non-native entry, or `state_components` — its machines seat some),
//   * the authored list does NOT carry its own generic native Transform
//     (that entry re-places and mirrors on its own materialization turn —
//     exactly one mirror, the authored value landing last).
// No-op otherwise: mirror-free content keeps byte-identical journals. State
// children stay out deliberately — their generic Transform refuses (`at:`
// owns child placement) and their `at:` lands post-flush; the documented
// split.
std::optional<base::Error>
mirror_seeded_transform(const SpawnOptions& options,
                        ecs::EntityRef entity,
                        const math::Transform& value,
                        const std::vector<GenericComponentEntry>& components,
                        bool state_components,
                        std::uint64_t cause_id);

} // namespace midday::loader
