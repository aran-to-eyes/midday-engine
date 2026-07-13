// core/loader/prefab_spawn.h — world.spawn(prefab, {at, overrides}) /
// world.despawn(ref): the RUNTIME (mid-tick, script-facing) prefab spawn
// path (m1-prefab-spawn, spec section 7 "Lifetime"). The loader's OWN
// load-time path (spawn_scene, core/loader/spawn.cpp) stays direct
// (world.spawn()); THIS class is what a script's queued spawn rides —
// queue_spawn reserves the EntityRef immediately (kPending, exit-test #3),
// and PrefabSpawner::realize — the post-flush half of the tick's TWO-PHASE
// structural-apply extension (core/tick/tick_loop.h; PrefabSpawner::prepare
// is the pre-flush despawn half, M2 0B D4) — actually builds the prefab
// subtree once World::flush_structural has made the reservation real.
//
// PREFAB-ONLY enforcement: this is the only public entry point a script (or
// any non-core caller) can reach to bring an entity into existence — it
// takes a prefab-ref (a resolved *.entity.yaml path) and NEVER a
// caller-assembled component list; there is no other public method here
// that mutates the World. Combined with scripts/check_entity_api.py (which
// forbids World::spawn/queue_spawn/emplace outside core/ + tests), a
// caller's only route to a live entity is through here or the loader —
// never a bare-entity assembly API (exit-test #4).
//
// Validation happens TWICE, on purpose: spawn() resolves the prefab file and
// applies every override NOW (structured refusal at the call site, mirroring
// core/ecs/structural_queue.h's "commands are validated when queued"
// contract — a script never gets back a handle for a request destined to
// fail deep in the tick); realize() only ever does WORK that was already
// validated to succeed, given the entity survives to the flush.
//
// Despawn semantics (M2 0B track D, FUSED-SPEC D4 — the linger queue lives
// HERE): despawn(ref, {after}) records a deadline; the actual removal still
// queues through the SAME ecs::World::queue_despawn every other despawn
// rides (children/subtree cascade is core/hierarchy's existing
// despawn-observer, untouched) — but only at the DUE tick's phase-8
// prepare(), after the full exit chains ran:
//   * ceiling tick = request_tick + ceil(after * ticks_per_second) — the
//     spec-literal IEEE double multiply then ceil (deliberately NOT
//     statechart::time_to_tick's llround: a corpse may linger a fraction of
//     a tick longer, never shorter). Negative / non-finite / overflowing
//     `after` refuses structurally ("despawn.bad_after"); an `after` with
//     no tick-rate source wired refuses ("despawn.no_tick_source").
//   * {after: 0} (and the plain 2-arg call) is immediate at the next
//     phase-8 cutoff: the CURRENT tick when requested before its prepare(),
//     the NEXT tick otherwise — a despawn requested from a phase-8 listener
//     lands next tick and never mutates the just-finished flush.
//   * Double-despawn is earliest-deadline-wins: an immediate advances an
//     existing later deadline, a later request never postpones an earlier
//     one, identical repeats are idempotent. A genuinely stale,
//     already-reaped ref still refuses ("ecs.stale_handle").
//   * The corpse stays FULLY alive — ticking, hearing events, transitioning
//     — through phases 1-7 of its due tick; phase 8 of that tick runs
//     prepare() (statechart exit chains -> base component onExit through
//     DespawnHooks -> queue_despawn), the flush removes it, and realize()
//     notes the REAP tick (G2), reaps the component seats, and fires
//     entity.despawned — in exactly that order.

#pragma once

#include "core/base/error.h"
#include "core/bus/bus.h"
#include "core/ecs/entity.h"
#include "core/journal/writer.h"
#include "core/loader/loader.h"
#include "core/loader/override.h"
#include "core/loader/prefab_instantiate.h"
#include "core/math/xform.h"
#include "core/reflect/registry.h"
#include "core/statechart/statechart.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// ecs::World and hierarchy::Hierarchy are already forward-declared by
// core/loader/loader.h (included above) — no need to repeat that block here.
namespace midday::loader {

struct PrefabSpawnResult {
    ecs::EntityRef ref; // kPending until the next structural-apply phase
    std::optional<base::Error> error;
};

// world.despawn's options bag (spec 4.2 / M2 #14): `after` in SECONDS.
// 0 (the default) is the immediate path — identical to the 2-arg call.
struct DespawnOptions {
    double after = 0.0;
};

class PrefabSpawner {
public:
    // All collaborators must outlive the PrefabSpawner. `events`/
    // `components_vocab` are the SAME project vocabulary the scene's own
    // prefabs were resolved against (loader::EventsDecl, loader::
    // ComponentVocab) — a prefab a script names is held to the identical
    // strictness contract as one authored directly in a scene. `loop` (M2
    // 0B track D) is the tick-rate source the linger ceiling needs
    // (ticks_per_second — the statechart's own TickLoop-collaborator
    // precedent); when left null, despawn with a non-zero `after` refuses
    // "despawn.no_tick_source" — fail-closed, never a silently-guessed
    // rate. Defaulted last so every pre-0B composition compiles unchanged.
    PrefabSpawner(ecs::World& world,
                  hierarchy::Hierarchy& hierarchy,
                  statechart::Statechart& chart,
                  bus::Bus& bus,
                  journal::Writer& journal,
                  reflect::Registry& registry,
                  const EventsDecl& events,
                  ComponentVocab components_vocab = ComponentVocab{},
                  const tick::TickLoop* loop = nullptr);

    PrefabSpawner(const PrefabSpawner&) = delete;
    PrefabSpawner& operator=(const PrefabSpawner&) = delete;
    PrefabSpawner(PrefabSpawner&&) = delete;
    PrefabSpawner& operator=(PrefabSpawner&&) = delete;
    ~PrefabSpawner() = default;

    // The script-facing spawn: loads (once; cached by path) and resolves
    // `prefab_path`'s machines against `overrides` NOW — refusing
    // "prefab.runtime_scripts_unsupported" when any resolved machine
    // declares state scripts (D3/G1, fail-closed: runtime spawn cannot seat
    // scripts until precompiled-artifact loading exists; components without
    // scripts are fine) — reserves the handle via ecs::World::queue_spawn,
    // and defers the actual build to the next realize() call. `at` becomes
    // the entity's local translation (the scene-format `prefab: ... at:`
    // precedent — a fresh identity transform, applied BEFORE the prefab's
    // `base:` list materializes through the dispatcher, exactly the
    // spawn_scene kPrefab order; a prefab authoring its own base Transform
    // replaces `at` wholesale, never composes). Named spawn_prefab, never
    // bare `spawn`
    // (scripts/check_entity_api.py treats ANY `.spawn(`/`->spawn(` call site
    // as suspect, any receiver — a second, distinct verb keeps every caller
    // of this method visibly PREFAB-scoped, never mistakable for the
    // bare-entity ecs::World::spawn the scanner actually polices).
    PrefabSpawnResult spawn_prefab(const std::string& prefab_path,
                                   const math::Vec3& at,
                                   const std::vector<OverrideEntry>& overrides = {});

    // The script-facing despawn (header contract above): records the
    // deadline; the ref stays FULLY alive until phase 8 of its due tick.
    // Refusals — all synchronous, at the request site: "ecs.stale_handle"
    // (already-reaped ref; a PENDING ref is a legal target — spawn-then-
    // despawn resolves in queue order at the flush, as before),
    // "despawn.bad_after" (negative / non-finite / overflowing seconds),
    // "despawn.no_tick_source" (non-zero `after` with no TickLoop wired).
    std::optional<base::Error> despawn(ecs::EntityRef ref, const DespawnOptions& opts = {});

    // Boot wiring (M2 0B track D): the script-host despawn lifecycle seam
    // (component_materialize.h DespawnHooks — ts/runtime's
    // ComponentInstanceHost in production). Unset: the exit chains still
    // run, the seat calls are skipped (a composition with no script
    // components has no seats to exit/reap).
    void set_despawn_hooks(DespawnHooks& hooks) { despawn_hooks_ = &hooks; }

    // Boot wiring (M2 0B track D): how realize() materializes — the SAME
    // SpawnOptions spawn_scene takes (component_materialize.h). The default
    // ({nullptr, eager}) reproduces pre-0B behavior for component-free
    // prefabs and FAIL-CLOSED refusals ("component.no_materializer") for
    // component-bearing ones; production wires {&instance_host,
    // defer_initial_entry: true} — realize() then starts each machine's
    // initial entry itself, right after its components seat (D2's split).
    void set_spawn_options(const SpawnOptions& options) { spawn_options_ = options; }

    [[nodiscard]] std::size_t pending_spawn_count() const { return pending_.size(); }

    // Despawn requests not yet realized: linger entries waiting on their
    // due tick plus (within phase 8) prepared reaps awaiting realize().
    [[nodiscard]] std::size_t pending_despawn_count() const {
        return lingers_.size() + queued_reaps_.size();
    }

    // Install as TickLoop::set_structural_preparer — the PRE-flush half
    // (M2 0B, D4). For every linger entry due at `tick` (request order):
    // journals ONE FLIGHT "prefab.despawn" record (citing the phase
    // marker), runs the full statechart exit chains
    // (Statechart::exit_host_machines — state-component onExit rides the
    // chart's exit-3 slot) and the base-component onExit
    // (DespawnHooks::despawn_exit) over the entity's live hierarchy
    // subtree, deepest entities first, and only then queues the despawn
    // into the flush that follows. Refused records abort FIRST ("no record,
    // no effect": a poisoned writer returns "despawn.journal_refused"
    // before any exit chain or queued removal). A ref that died by another
    // route since its request drops with no effect but never silently — a
    // FLIGHT "prefab.despawn_stale" breadcrumb (entity, requested, due)
    // journals the dropped linger; a still-PENDING ref (spawned and
    // despawned the same tick) queues without exit chains — nothing was
    // ever materialized. A despawn requested from inside these exit chains
    // lands NEXT tick (the cutoff rule) and never mutates this walk.
    std::optional<base::Error> prepare(std::uint64_t tick, std::uint64_t phase_record_id);

    // Install as TickLoop::set_structural_realizer — the POST-flush half.
    // Order per despawn prepared this tick: DespawnHooks::note_despawn
    // (the actual REAP tick — the 0A G2 closure) -> reap_entity (seat
    // cleanup, children before root) -> the entity.despawned trigger
    // (citing the prefab.despawn record — AFTER the exit chains by
    // construction). Then adopts + materializes (core/loader/
    // prefab_instantiate.h) every pending spawn that survived to the
    // flush, firing entity.spawned once its machines' enter chains have
    // run. Spawns requested from inside realize (a phase-8 listener) land
    // NEXT tick — the same re-entrancy cutoff despawns follow.
    std::optional<base::Error> realize(std::uint64_t phase_record_id);

private:
    struct PendingSpawn {
        ecs::EntityRef ref;
        std::string prefab_path;
        math::Vec3 at;
        std::vector<MachineFile> machines; // overrides already applied (spawn()-time)
    };

    // One accepted despawn request (the linger queue). Insertion order is
    // request order — the phase-8 processing order for same-tick deadlines.
    struct LingerEntry {
        ecs::EntityRef ref;
        std::uint64_t request_tick = 0;
        std::uint64_t due_tick = 0;
    };

    // prepare() -> realize() handoff, same tick: a despawn whose exit
    // chains ran and whose removal sits in the flush between the halves.
    struct QueuedReap {
        ecs::EntityRef ref;                  // the requested root
        std::vector<ecs::EntityRef> subtree; // live subtree at prepare, pre-order (root first)
        std::uint64_t despawn_record = 0;    // the prefab.despawn journal id
    };

    // Loads (once) and caches `prefab_path`; nullptr + *error on failure.
    const EntityFile* catalog(const std::string& prefab_path, std::optional<base::Error>& error);

    // Pre-order hierarchy walk (root first, children depth-first): the
    // exact set the flush's despawn cascade will remove, captured while it
    // is still alive. Reversed by the callers for exit/reap (deepest-first
    // — the mirror of construction).
    void collect_subtree(ecs::EntityRef root, std::vector<ecs::EntityRef>& out) const;

    ecs::World* world_;
    hierarchy::Hierarchy* hierarchy_;
    statechart::Statechart* chart_;
    bus::Bus* bus_;
    journal::Writer* journal_;
    reflect::Registry* registry_;
    const EventsDecl* events_;
    ComponentVocab components_vocab_;
    const tick::TickLoop* loop_ = nullptr;  // tick-rate source (may be null)
    DespawnHooks* despawn_hooks_ = nullptr; // script-host seat seam (may be null)
    SpawnOptions spawn_options_;            // how realize() materializes
    std::unordered_map<std::string, EntityFile> catalog_;
    std::vector<PendingSpawn> pending_;
    std::vector<LingerEntry> lingers_;     // the linger queue (despawn())
    std::vector<QueuedReap> queued_reaps_; // prepare -> realize, same tick
    std::uint64_t last_prepared_tick_ = 0; // the phase-8 cutoff marker
};

} // namespace midday::loader
