// core/loader/prefab_spawn.cpp — see prefab_spawn.h for the header contract
// this implements.

#include "core/loader/prefab_spawn.h"

#include "core/ecs/world.h"
#include "core/hierarchy/hierarchy.h"
#include "core/tick/tick_loop.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <ranges>
#include <utility>

namespace midday::loader {

namespace {

base::Json ref_bits(ecs::EntityRef ref) {
    return {static_cast<std::int64_t>(ref.to_bits())};
}

base::Error prefab_not_found_error(const std::string& path, const base::Error& cause) {
    base::Error error;
    error.code = "prefab.not_found";
    error.message = "prefab '" + path + "': [" + cause.code + "] " + cause.message;
    error.details.set("path", path);
    error.details.set("cause", cause.to_json());
    return error;
}

// D3 (G1, fail-closed): a runtime-spawned prefab that declares state
// scripts refuses in the validation window — no handle reserved, no journal
// effect, and no module compilation ever reaches phase 8. Seating deferred:
// the node that defines precompiled-artifact loading owns it (LEDGER).
base::Error runtime_scripts_error(const std::string& prefab_path, base::Json scripts) {
    base::Error error;
    error.code = "prefab.runtime_scripts_unsupported";
    error.message = "prefab '" + prefab_path +
                    "' declares state scripts: runtime spawn cannot seat them until "
                    "precompiled-artifact loading exists (a later node) — spawn it from "
                    "a scene, or strip its scripts";
    error.details.set("prefab", prefab_path);
    error.details.set("scripts", std::move(scripts));
    return error;
}

base::Error bad_after_error(double after, std::string_view reason) {
    base::Error error;
    error.code = "despawn.bad_after";
    error.message = "despawn {after} must be a finite, non-negative, representable number "
                    "of seconds";
    error.details.set("reason", reason);
    if (std::isfinite(after))
        error.details.set("after", base::Json(after));
    return error;
}

base::Error no_tick_source_error() {
    base::Error error;
    error.code = "despawn.no_tick_source";
    error.message = "despawn {after} needs the tick rate: wire the TickLoop into the "
                    "PrefabSpawner (constructor) before requesting a linger";
    return error;
}

// No record, no effect (the bus.trigger rule, prefab_instantiate.cpp's
// prefab.journal_refused sibling): a refused prefab.despawn record aborts
// the despawn before ANY effect runs.
base::Error journal_refused_error(const journal::Writer& journal) {
    base::Error error{.code = "despawn.journal_refused",
                      .message = "the journal refused the prefab.despawn record"};
    const std::optional<base::Error>& status = journal.status();
    if (status.has_value())
        error.details.set("journal", status->to_json());
    return error;
}

} // namespace

PrefabSpawner::PrefabSpawner(ecs::World& world,
                             hierarchy::Hierarchy& hierarchy,
                             statechart::Statechart& chart,
                             bus::Bus& bus,
                             journal::Writer& journal,
                             reflect::Registry& registry,
                             const EventsDecl& events,
                             ComponentVocab components_vocab,
                             const tick::TickLoop* loop)
    : world_(&world), hierarchy_(&hierarchy), chart_(&chart), bus_(&bus), journal_(&journal),
      registry_(&registry), events_(&events), components_vocab_(std::move(components_vocab)),
      loop_(loop) {}

const EntityFile* PrefabSpawner::catalog(const std::string& prefab_path,
                                         std::optional<base::Error>& error) {
    const auto found = catalog_.find(prefab_path);
    if (found != catalog_.end())
        return &found->second;
    EntityLoadResult loaded =
        load_entity_file(prefab_path, *registry_, *events_, components_vocab_, /*lenient=*/false);
    if (loaded.error.has_value()) {
        error = prefab_not_found_error(prefab_path, *loaded.error);
        return nullptr;
    }
    if (!loaded.entity.has_value()) { // defensive: loaders return one or the other
        error = base::Error{.code = "loader.bad_ref", .message = "entity load failed"};
        return nullptr;
    }
    const auto [it, inserted] = catalog_.emplace(prefab_path, std::move(*loaded.entity));
    (void)inserted;
    return &it->second;
}

PrefabSpawnResult PrefabSpawner::spawn_prefab(const std::string& prefab_path,
                                              const math::Vec3& at,
                                              const std::vector<OverrideEntry>& overrides) {
    PrefabSpawnResult out;
    const EntityFile* entity_file = catalog(prefab_path, out.error);
    if (out.error.has_value())
        return out;

    ResolvedMachinesResult resolved = resolve_prefab_machines(*entity_file, overrides, prefab_path);
    if (resolved.error.has_value()) {
        out.error = std::move(resolved.error);
        return out;
    }

    // D3 (G1): scan the RESOLVED machines — post-resolution, pre-queue, the
    // same validation window every other synchronous refusal here uses.
    // Prefabs with components but no scripts pass: the dispatcher
    // materializes those (realize below).
    base::Json script_refs = base::Json::array();
    bool any_scripts = false;
    for (const MachineFile& machine : resolved.machines) {
        for (const StateScriptRef& script : machine.scripts) {
            script_refs.push(base::Json(script.ref)); // as authored — the diagnostic form
            any_scripts = true;
        }
    }
    if (any_scripts) {
        out.error = runtime_scripts_error(prefab_path, std::move(script_refs));
        return out;
    }

    const ecs::EntityRef ref = world_->queue_spawn();
    pending_.push_back(PendingSpawn{ref, prefab_path, at, std::move(resolved.machines)});
    out.ref = ref;
    return out;
}

std::optional<base::Error> PrefabSpawner::despawn(ecs::EntityRef ref, const DespawnOptions& opts) {
    // Stale already-reaped refs refuse NOW, at the request site (the
    // spawn_prefab synchronous-validation symmetry). A PENDING ref (queued
    // spawn, not yet flushed) is a legal target: spawn-then-despawn
    // resolves in queue order at the flush — check_alive spells that state
    // "ecs.entity_pending", which is exactly the one non-alive state we
    // accept.
    if (auto error = world_->check_alive(ref)) {
        if (error->code != "ecs.entity_pending")
            return error; // ecs.stale_handle
    }

    const std::uint64_t request_tick = bus_->tick();
    std::uint64_t due_tick = request_tick;
    if (opts.after != 0.0) {
        // ceiling = request_tick + ceil(after * ticks_per_second) — the
        // D4 spec-literal double multiply + ceil (prefab_spawn.h header
        // note contrasts this with time_to_tick's llround, on purpose).
        if (std::isnan(opts.after) || std::isinf(opts.after))
            return bad_after_error(opts.after, "non_finite");
        if (opts.after < 0.0)
            return bad_after_error(opts.after, "negative");
        if (loop_ == nullptr)
            return no_tick_source_error();
        const double scaled =
            std::ceil(opts.after * static_cast<double>(loop_->ticks_per_second()));
        // Overflow fail-closed: the delay must survive the double->integer
        // conversion AND the addition below. (int64 max keeps the cast
        // well-defined; no sim lives 2^63 ticks.)
        if (!(scaled <= static_cast<double>(std::numeric_limits<std::int64_t>::max())))
            return bad_after_error(opts.after, "overflow");
        const auto delay_ticks = static_cast<std::uint64_t>(scaled);
        if (delay_ticks > std::numeric_limits<std::uint64_t>::max() - request_tick)
            return bad_after_error(opts.after, "overflow");
        due_tick = request_tick + delay_ticks;
    }
    // The phase-8 cutoff (re-entrancy rule): a deadline at or before the
    // tick prepare() already processed can never join that flush — it lands
    // at the NEXT cutoff. (Requests during phases 1-7 keep their own tick:
    // last_prepared_tick_ is still the previous tick's.)
    if (due_tick <= last_prepared_tick_)
        due_tick = last_prepared_tick_ + 1;

    // Earliest-deadline-wins merge: an immediate advances an existing
    // later deadline; a later (or identical) request is an idempotent
    // no-op — it never postpones.
    for (LingerEntry& entry : lingers_) {
        if (entry.ref == ref) {
            if (due_tick < entry.due_tick)
                entry.due_tick = due_tick;
            return std::nullopt;
        }
    }
    lingers_.push_back(LingerEntry{ref, request_tick, due_tick});
    return std::nullopt;
}

void PrefabSpawner::collect_subtree(ecs::EntityRef root, std::vector<ecs::EntityRef>& out) const {
    out.push_back(root);
    for (ecs::EntityRef child = hierarchy_->first_child_of(root); !child.is_null();
         child = hierarchy_->next_sibling_of(child))
        collect_subtree(child, out);
}

std::optional<base::Error> PrefabSpawner::prepare(std::uint64_t tick,
                                                  std::uint64_t phase_record_id) {
    // The cutoff stamp FIRST: any despawn requested from the exit chains
    // below computes its deadline against this tick and lands next tick.
    last_prepared_tick_ = tick;
    if (lingers_.empty())
        return std::nullopt;

    // Partition due from not-yet-due BEFORE any hook runs, preserving
    // request order — the walk below iterates a local list, so re-entrant
    // despawn() calls (which push onto the fresh lingers_) never mutate it.
    std::vector<LingerEntry> due;
    std::vector<LingerEntry> keep;
    for (LingerEntry& entry : lingers_)
        (entry.due_tick <= tick ? due : keep).push_back(entry);
    lingers_ = std::move(keep);

    for (const LingerEntry& entry : due) {
        if (!world_->alive(entry.ref)) {
            const std::optional<base::Error> state = world_->check_alive(entry.ref);
            if (state.has_value() && state->code != "ecs.entity_pending") {
                // Genuinely stale: another route reaped the ref since its
                // request — there is nothing left to despawn and no tick at
                // which we did it, so the entry drops without effect. But
                // never silently: the FLIGHT breadcrumb lets a journal
                // reader tell "linger superseded" from "linger lost". No
                // effect follows this record, so a refused write (sticky
                // writer) has nothing to abort.
                base::Json stale_payload = base::Json::object();
                stale_payload.set("entity", ref_bits(entry.ref));
                stale_payload.set("requested", static_cast<std::int64_t>(entry.request_tick));
                stale_payload.set("due", static_cast<std::int64_t>(entry.due_tick));
                (void)journal_->record(tick,
                                       journal::Tier::Flight,
                                       "prefab.despawn_stale",
                                       phase_record_id,
                                       std::move(stale_payload));
                continue;
            }
            // Still PENDING: spawned this very tick — queue the despawn
            // behind its spawn command (queue order resolves born-then-dead
            // at the flush, the pre-linger behavior). Nothing materialized,
            // so there are no chains to exit. Record BEFORE the queue: a
            // writer refusal aborts the despawn with no effect executed.
            base::Json payload = base::Json::object();
            payload.set("entity", ref_bits(entry.ref));
            payload.set("requested", static_cast<std::int64_t>(entry.request_tick));
            payload.set("due", static_cast<std::int64_t>(entry.due_tick));
            const std::uint64_t record_id = journal_->record(
                tick, journal::Tier::Flight, "prefab.despawn", phase_record_id, std::move(payload));
            if (record_id == 0)
                return journal_refused_error(*journal_);
            if (auto error = world_->queue_despawn(entry.ref))
                return error; // unreachable: pending refs queue behind their spawn
            queued_reaps_.push_back(QueuedReap{entry.ref, {entry.ref}, record_id});
            continue;
        }

        // Record before effect: THE despawn record — the cause of the exit
        // chains, the base onExit hooks, and the entity.despawned event. A
        // refusal (0) aborts HERE, before any effect: no exit chain runs, no
        // hook fires, no removal queues — unjournaled despawns do not exist.
        base::Json payload = base::Json::object();
        payload.set("entity", ref_bits(entry.ref));
        payload.set("requested", static_cast<std::int64_t>(entry.request_tick));
        payload.set("due", static_cast<std::int64_t>(entry.due_tick));
        const std::uint64_t record_id = journal_->record(
            tick, journal::Tier::Flight, "prefab.despawn", phase_record_id, std::move(payload));
        if (record_id == 0)
            return journal_refused_error(*journal_);

        // The live subtree, captured pre-flush: exactly the set the flush's
        // despawn cascade removes. Pre-order (root first) — walked in
        // REVERSE below, deepest entities first, the mirror of construction.
        QueuedReap reap;
        reap.ref = entry.ref;
        reap.despawn_record = record_id;
        collect_subtree(entry.ref, reap.subtree);

        // Full statechart exit chains (D4 order, step 1): state scripts +
        // state-component onExit ride the chart's own A.2.1 exit template
        // (exit-3 = reverse attach). The corpse is still fully alive here.
        for (const ecs::EntityRef doomed : std::ranges::reverse_view(reap.subtree))
            chart_->exit_host_machines(doomed, record_id);
        // Base component onExit (step 2): reverse attach order per entity,
        // deepest entities first — the DespawnHooks seam.
        if (despawn_hooks_ != nullptr) {
            for (const ecs::EntityRef doomed : std::ranges::reverse_view(reap.subtree))
                despawn_hooks_->despawn_exit(doomed, record_id);
        }
        // Queue the removal (step 3) into the flush that follows: the ONE
        // root command — core/hierarchy's despawn observer cascades the
        // subtree, untouched.
        if (auto error = world_->queue_despawn(entry.ref))
            return error; // unreachable: alive above; fail loud, not silent
        queued_reaps_.push_back(std::move(reap));
    }
    return std::nullopt;
}

std::optional<base::Error> PrefabSpawner::realize(std::uint64_t phase_record_id) {
    const std::uint64_t tick = bus_->tick();

    // Despawn realization (D4 post-flush): every despawn prepare() queued
    // this tick — the flush has made the removal real. Order per entry:
    // note_despawn (the REAP tick — the 0A G2 closure) -> reap_entity (seat
    // cleanup, children before root) -> entity.despawned (citing the
    // prefab.despawn record; the exit chains ran at prepare, so the event
    // is after them by construction).
    for (const QueuedReap& reap : queued_reaps_) {
        if (world_->alive(reap.ref))
            continue; // defensive: the flush applies every queued despawn
        if (despawn_hooks_ != nullptr) {
            for (const ecs::EntityRef reaped : std::ranges::reverse_view(reap.subtree)) {
                despawn_hooks_->note_despawn(reaped, tick);
                despawn_hooks_->reap_entity(reaped);
            }
        }
        base::Json payload = base::Json::object();
        payload.set("entity", ref_bits(reap.ref));
        (void)bus_->trigger(bus::EventKey::entity(reap.ref),
                            base::Name("entity.despawned"),
                            payload,
                            reap.despawn_record);
    }
    queued_reaps_.clear();

    struct PendingChild {
        ecs::EntityRef ref;
        math::Transform at;
    };

    std::vector<PendingChild> pending_children;

    // Drain-swap (the input-phase cutoff pattern): a spawn requested from a
    // phase-8 listener below lands in the fresh pending_ and realizes NEXT
    // tick — its own queue_spawn command flushes next tick anyway, and the
    // old in-place iteration could never survive the push_back.
    std::vector<PendingSpawn> draining;
    draining.swap(pending_);

    for (PendingSpawn& spawn_request : draining) {
        if (!world_->alive(spawn_request.ref))
            continue; // dropped: despawned this same tick, before this flush

        if (auto error = hierarchy_->adopt(spawn_request.ref))
            return error;
        math::Transform local;
        local.translation = spawn_request.at;
        if (auto error = hierarchy_->set_local(spawn_request.ref, local))
            return error;

        // The catalog entry outlives us (never evicted); base components
        // apply verbatim — overrides are machine-scoped by grammar.
        std::optional<base::Error> catalog_error;
        const EntityFile* entity_file = catalog(spawn_request.prefab_path, catalog_error);
        if (catalog_error.has_value())
            return catalog_error;

        MaterializeResult materialized = materialize_prefab(*world_,
                                                            *hierarchy_,
                                                            *chart_,
                                                            *journal_,
                                                            tick,
                                                            spawn_request.machines,
                                                            spawn_request.ref,
                                                            spawn_request.prefab_path,
                                                            phase_record_id,
                                                            spawn_options_,
                                                            &entity_file->base_components);
        if (materialized.error.has_value())
            return materialized.error;
        for (const PrefabChild& child : materialized.children)
            pending_children.push_back(PendingChild{child.ref, child.at});

        // The deferred half of the D2 split: state components seated inside
        // materialize_prefab, no state scripts by construction (G1) — the
        // initial enter chains start here, exactly where the eager path ran
        // them.
        if (spawn_options_.defer_initial_entry) {
            for (const MaterializedMachine& machine : materialized.machines) {
                if (auto error = chart_->start_initial_entry(machine.id))
                    return error;
            }
        }

        // "entity.spawned ... after its initial states entered" (builtin_events.cpp):
        // fired once every one of this root's machines has run its enter
        // chain. A top-level world.spawn() is always a root (no `parent`
        // param on the public surface) — parent reads null.
        base::Json spawned_payload = base::Json::object();
        spawned_payload.set("entity", ref_bits(spawn_request.ref));
        spawned_payload.set("parent", ref_bits(ecs::EntityRef{}));
        (void)bus_->trigger(bus::EventKey::entity(spawn_request.ref),
                            base::Name("entity.spawned"),
                            spawned_payload,
                            phase_record_id);
    }

    if (!pending_children.empty()) {
        if (auto error = world_->flush_structural())
            return error;
        for (const PendingChild& child : pending_children)
            (void)hierarchy_->set_local(child.ref, child.at);
    }
    return std::nullopt;
}

} // namespace midday::loader
