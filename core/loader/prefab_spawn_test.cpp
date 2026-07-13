// loader.prefab_spawn — m1-prefab-spawn's exit tests (honest, literal):
//   1. 100 prefab fixtures spawn MID-TICK; all go live at PHASE 8 with an
//      enter-chain trace (journal causal chain: tick.phase(structural-apply)
//      -> prefab.spawn -> statechart.instantiate -> the live enter-chain
//      state, verified behaviorally through Statechart::in_state).
//   2. A despawn queued mid-query (inside a live world.view().each()) does
//      NOT invalidate that iteration.
//   3. Handles read alive == false right up until the tick's structural-
//      apply phase, in BOTH directions (spawn and despawn).
//   4. The entity-API scanner (scripts/check_entity_api.py) — nothing here
//      calls World::spawn/queue_spawn/emplace from outside core/, so this
//      file changes nothing about that gate; asserted by verify.sh directly.

#include "core/base/file_io.h"
#include "core/bus/test_support.h"
#include "core/loader/prefab_spawn.h"
#include "core/loader/prefab_test_support.h"
#include "core/statechart/test_support.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace midday;
using midday::base::Name;
using midday::bus::test::RecordingListener;
using midday::ecs::EntityRef;
using midday::journal::Record;
using midday::loader::EventsDecl;
using midday::loader::PrefabSpawner;
using midday::loader::PrefabSpawnResult;
using midday::loader::test::PrefabFixture; // shared two-phase spawner fixture
using midday::loader::test::write_goblin_prefab;
using midday::statechart::test::ChartFixture;
using midday::statechart::test::of_kind; // journal-record filter, already shared cross-module
using midday::testkit::unwrap;

namespace {

const Record* find_by_id(const std::vector<Record>& records, std::uint64_t id) {
    for (const Record& record : records)
        if (record.id == id)
            return &record;
    return nullptr;
}

// Fires 100 spawn() calls the moment the phase runs — install on kUpdate
// (an OPEN phase, well before structural-apply) so the 100 requests are
// genuinely queued MID-TICK, not before the tick started.
struct SpawnHook final : tick::PhaseHook {
    PrefabSpawner* spawner;
    std::string prefab_path;
    std::vector<EntityRef> refs;
    bool fired = false;

    void on_phase(tick::TickLoop&, const tick::PhaseContext&) override {
        fired = true;
        for (int i = 0; i < 100; ++i) {
            PrefabSpawnResult result = spawner->spawn_prefab(prefab_path, math::Vec3{}, {});
            REQUIRE_FALSE(result.error.has_value());
            REQUIRE_FALSE(result.ref.is_null());
            refs.push_back(result.ref);
        }
    }
};

} // namespace

TEST_CASE("loader.prefab_spawn: 100 prefabs spawn mid-tick, go live at phase 8 with an "
          "enter-chain trace") {
    PrefabFixture pf;
    const std::string prefab_path = write_goblin_prefab(pf.fix.dir);

    SpawnHook hook;
    hook.spawner = &pf.spawner;
    hook.prefab_path = prefab_path;
    REQUIRE_FALSE(pf.fix.loop().add_hook(tick::Phase::kUpdate, hook).has_value());

    REQUIRE_FALSE(pf.fix.loop().tick().has_value());
    REQUIRE(hook.fired);
    REQUIRE(hook.refs.size() == 100);

    // Exit-test #1 + #3: every handle queued mid-tick is alive NOW (after
    // the tick's own structural-apply phase completed) — and, per the
    // hierarchy build_subtree contract, its ONE hierarchy child is the
    // machine root, from which the enter chain is verifiable behaviorally
    // (not just journaled).
    for (const EntityRef ref : hook.refs) {
        CHECK(pf.fix.world.alive(ref));
        const EntityRef machine_root = pf.fix.hierarchy.first_child_of(ref);
        REQUIRE_FALSE(machine_root.is_null());
        const auto* root_component = pf.fix.world.try_get<statechart::MachineRoot>(machine_root);
        REQUIRE(root_component != nullptr);
        CHECK(pf.fix.chart().in_state(root_component->machine, Name("main"), Name("Idle")));
    }

    // The enter-chain TRACE: a connected causal journal chain from the
    // tick's structural-apply phase marker down through this spawn's own
    // prefab.spawn record to the statechart's own instantiate record (which
    // is itself the enter chain's root per Statechart::instantiate's
    // contract — "journals ... enters the initial chains").
    std::vector<Record> records = pf.fix.finish();
    CHECK(of_kind(records, "prefab.spawn").size() == 100);
    const std::vector<Record> instantiated = of_kind(records, "statechart.instantiate");
    REQUIRE(instantiated.size() == 100);
    for (const Record& instantiate_record : instantiated) {
        const Record* spawn_record = find_by_id(records, instantiate_record.cause_id);
        REQUIRE(spawn_record != nullptr);
        CHECK(spawn_record->kind == "prefab.spawn");
        const Record* phase_record = find_by_id(records, spawn_record->cause_id);
        REQUIRE(phase_record != nullptr);
        CHECK(phase_record->kind == "tick.phase");
        CHECK(phase_record->payload.find("phase")->as_string() == "structural-apply");
    }
}

TEST_CASE("loader.prefab_spawn: despawn mid-query does not invalidate iteration") {
    PrefabFixture pf;
    const std::string prefab_path = write_goblin_prefab(pf.fix.dir);

    std::vector<EntityRef> roots;
    for (int i = 0; i < 10; ++i) {
        PrefabSpawnResult result = pf.spawner.spawn_prefab(prefab_path, math::Vec3{}, {});
        REQUIRE_FALSE(result.error.has_value());
        roots.push_back(result.ref);
    }
    REQUIRE_FALSE(pf.fix.loop().tick().has_value()); // realize: all 10 alive
    for (const EntityRef ref : roots)
        REQUIRE(pf.fix.world.alive(ref));

    // Iterate a live pool (every goblin's machine root carries StateNode via
    // its one state) and, mid-iteration, queue a despawn of the FIRST
    // goblin's root through the SAME public API the exit test names.
    std::uint32_t visited = 0;
    std::optional<base::Error> despawn_error;
    pf.fix.world.view<statechart::StateNode>().each(
        [&](EntityRef /*state_entity*/, statechart::StateNode& /*node*/) {
            ++visited;
            if (visited == 1)
                despawn_error = pf.spawner.despawn(roots.front());
        });

    REQUIRE_FALSE(despawn_error.has_value());
    CHECK(visited == 10); // the iteration ran to completion, uninterrupted

    // Exit-test #3 (despawn direction): still alive — queued, not applied.
    CHECK(pf.fix.world.alive(roots.front()));

    REQUIRE_FALSE(pf.fix.loop().tick().has_value()); // flush: the despawn lands
    CHECK_FALSE(pf.fix.world.alive(roots.front()));
    for (std::size_t i = 1; i < roots.size(); ++i)
        CHECK(pf.fix.world.alive(roots[i])); // the OTHER nine are untouched
}

TEST_CASE("loader.prefab_spawn: handles read alive == false until phase 8, spawn and despawn") {
    PrefabFixture pf;
    const std::string prefab_path = write_goblin_prefab(pf.fix.dir);

    PrefabSpawnResult spawned = pf.spawner.spawn_prefab(prefab_path, math::Vec3{}, {});
    REQUIRE_FALSE(spawned.error.has_value());
    CHECK_FALSE(pf.fix.world.alive(spawned.ref)); // pending: reserved, not alive

    REQUIRE_FALSE(pf.fix.loop().tick().has_value());
    CHECK(pf.fix.world.alive(spawned.ref)); // alive only after phase 8

    REQUIRE_FALSE(pf.spawner.despawn(spawned.ref).has_value());
    CHECK(pf.fix.world.alive(spawned.ref)); // still alive: despawn queued, not applied

    REQUIRE_FALSE(pf.fix.loop().tick().has_value());
    CHECK_FALSE(pf.fix.world.alive(spawned.ref)); // dead only after phase 8
}

TEST_CASE("loader.prefab_spawn: lifecycle events fire at phase 8, keyed on the entity") {
    PrefabFixture pf;
    const std::string prefab_path = write_goblin_prefab(pf.fix.dir);

    PrefabSpawnResult spawned = pf.spawner.spawn_prefab(prefab_path, math::Vec3{}, {});
    REQUIRE_FALSE(spawned.error.has_value());

    std::vector<std::string> log;
    RecordingListener listener("goblin", log);
    REQUIRE_FALSE(pf.fix.bus().subscribe(listener, bus::EventKey::entity(spawned.ref)).has_value());

    REQUIRE_FALSE(pf.fix.loop().tick().has_value());
    CHECK(log == std::vector<std::string>{"goblin:entity.spawned"});

    REQUIRE_FALSE(pf.spawner.despawn(spawned.ref).has_value());
    REQUIRE_FALSE(pf.fix.loop().tick().has_value());
    CHECK(log == std::vector<std::string>{"goblin:entity.spawned", "goblin:entity.despawned"});
}

TEST_CASE("loader.prefab_spawn: a bad override path refuses synchronously, before any handle "
          "is reserved") {
    PrefabFixture pf;
    const std::string prefab_path = write_goblin_prefab(pf.fix.dir);

    // "goblin" (the machine name) must lead the path — split_overrides_for_
    // machine strips it before apply_overrides ever sees the rest; "nope"
    // would just land in the unmatched bucket and silently do nothing.
    const loader::OverrideEntry bad{.path = "goblin/NoSuchRegion/sequence"};
    PrefabSpawnResult spawned = pf.spawner.spawn_prefab(prefab_path, math::Vec3{}, {bad});
    REQUIRE(spawned.error.has_value());
    CHECK(unwrap(spawned.error).code == "loader.bad_ref");
    CHECK(spawned.ref.is_null());
    CHECK(pf.spawner.pending_spawn_count() == 0);
}

TEST_CASE("loader.prefab_spawn: an unresolvable prefab path refuses with prefab.not_found") {
    PrefabFixture pf;
    PrefabSpawnResult spawned =
        pf.spawner.spawn_prefab(pf.fix.dir.file("nope.entity.yaml"), math::Vec3{});
    REQUIRE(spawned.error.has_value());
    CHECK(unwrap(spawned.error).code == "prefab.not_found");
    CHECK(spawned.ref.is_null());
}

// ---- M2 0B track D: the G1 refusal + its counter-case ----------------------
// (The despawn-linger battery lives in core/loader/despawn_linger_test.cpp.)

namespace {

struct RecordingMaterializer final : loader::ScriptComponentMaterializer {
    std::vector<std::string> log;

    std::optional<base::Error> materialize_base(EntityRef entity,
                                                const loader::GenericComponentEntry& entry,
                                                std::uint64_t) override {
        log.push_back("base:" + std::string(entry.type.view()) + "@" +
                      std::to_string(entity.index));
        return std::nullopt;
    }

    std::optional<base::Error> materialize_state(statechart::Statechart&,
                                                 statechart::MachineId,
                                                 base::Name,
                                                 base::Name state,
                                                 EntityRef,
                                                 const statechart::StateComponentDesc& desc,
                                                 std::uint64_t) override {
        log.push_back("state:" + std::string(desc.type.view()) + "@" + std::string(state.view()));
        return std::nullopt;
    }

    std::optional<base::Error>
    mirror_native_transform(EntityRef, const math::Transform&, std::uint64_t) override {
        log.emplace_back("mirror");
        return std::nullopt;
    }
};

// A goblin whose machine carries hookable content and components: Idle owns
// a state component; the prefab owns a base component. Names come from a
// manifest-less vocab the test supplies directly.

std::string write_component_goblin(const testkit::TempDir& dir) {
    REQUIRE_FALSE(base::write_file(dir.file("cgoblin.machine.yaml"),
                                   "format: 1\n"
                                   "machine: cgoblin\n"
                                   "regions:\n"
                                   "  main:\n"
                                   "    initial: Idle\n"
                                   "    states:\n"
                                   "      Idle:\n"
                                   "        components:\n"
                                   "          - Aura: {}\n",
                                   "t")
                      .has_value());
    const std::string entity_path =
        std::filesystem::path(dir.file("cgoblin.entity.yaml")).generic_string();
    REQUIRE_FALSE(base::write_file(entity_path,
                                   "format: 1\n"
                                   "entity: ComponentGoblin\n"
                                   "base:\n"
                                   "  - Pulse: {}\n"
                                   "machines:\n"
                                   "  - instance: {path: cgoblin.machine.yaml}\n",
                                   "t")
                      .has_value());
    return entity_path;
}

// A goblin whose machine declares a state SCRIPT — the G1 refusal target.

std::string write_scripted_goblin(const testkit::TempDir& dir) {
    REQUIRE_FALSE(
        base::write_file(dir.file("idle.ts"), "export default class Idle {}\n", "t").has_value());
    REQUIRE_FALSE(base::write_file(dir.file("sgoblin.machine.yaml"),
                                   "format: 1\n"
                                   "machine: sgoblin\n"
                                   "regions:\n"
                                   "  main:\n"
                                   "    initial: Idle\n"
                                   "    states:\n"
                                   "      Idle: {script: idle.ts}\n",
                                   "t")
                      .has_value());
    const std::string entity_path =
        std::filesystem::path(dir.file("sgoblin.entity.yaml")).generic_string();
    REQUIRE_FALSE(base::write_file(entity_path,
                                   "format: 1\n"
                                   "entity: ScriptedGoblin\n"
                                   "machines:\n"
                                   "  - instance: {path: sgoblin.machine.yaml}\n",
                                   "t")
                      .has_value());
    return entity_path;
}

// entity.despawned event.trigger records, journal order.

} // namespace

TEST_CASE("loader.prefab_spawn: G1 fail-closed — a runtime prefab with state scripts refuses "
          "prefab.runtime_scripts_unsupported before any handle is reserved") {
    PrefabFixture pf;
    const std::string prefab_path = write_scripted_goblin(pf.fix.dir);

    const std::uint32_t alive_before = pf.fix.world.alive_count();
    PrefabSpawnResult spawned = pf.spawner.spawn_prefab(prefab_path, math::Vec3{});
    REQUIRE(spawned.error.has_value());
    CHECK(unwrap(spawned.error).code == "prefab.runtime_scripts_unsupported");
    CHECK(unwrap(spawned.error).details.find("prefab")->as_string() == prefab_path);
    const base::Json* scripts = unwrap(spawned.error).details.find("scripts");
    REQUIRE(scripts != nullptr);
    REQUIRE(scripts->elements().size() == 1);
    CHECK(scripts->elements()[0].as_string() == "idle.ts");

    // Fail-closed means NOTHING happened: no handle, no pending request,
    // no journal effect once the tick runs.
    CHECK(spawned.ref.is_null());
    CHECK(pf.spawner.pending_spawn_count() == 0);
    CHECK(pf.fix.world.alive_count() == alive_before);
    REQUIRE_FALSE(pf.fix.loop().tick().has_value());
    std::vector<Record> records = pf.fix.finish();
    CHECK(of_kind(records, "prefab.spawn").empty());
    CHECK(of_kind(records, "statechart.instantiate").empty());
}

TEST_CASE("loader.prefab_spawn: a runtime prefab with components but NO scripts materializes "
          "fully through the dispatcher (the G1 counter-case)") {
    // Vocabulary with the two authored component names, dispatcher options
    // wired the production way ({materializer, defer_initial_entry}).
    loader::ComponentVocab vocab;
    vocab.extracted = {"Pulse", "Aura"};

    ChartFixture fix;
    EventsDecl events;
    RecordingMaterializer materializer;
    PrefabSpawner spawner(fix.world,
                          fix.hierarchy,
                          fix.chart(),
                          fix.bus(),
                          fix.writer(),
                          fix.registry,
                          events,
                          vocab,
                          &fix.loop());
    loader::SpawnOptions options;
    options.scripts = &materializer;
    options.defer_initial_entry = true;
    spawner.set_spawn_options(options);
    fix.loop().set_structural_preparer([&](std::uint64_t tick, std::uint64_t phase_record_id) {
        return spawner.prepare(tick, phase_record_id);
    });
    fix.loop().set_structural_realizer(
        [&](std::uint64_t phase_record_id) { return spawner.realize(phase_record_id); });

    const std::string prefab_path = write_component_goblin(fix.dir);
    PrefabSpawnResult spawned = spawner.spawn_prefab(prefab_path, math::Vec3{});
    REQUIRE_FALSE(spawned.error.has_value());

    std::vector<std::string> bus_log;
    RecordingListener listener("g", bus_log);
    REQUIRE_FALSE(fix.bus().subscribe(listener, bus::EventKey::entity(spawned.ref)).has_value());

    REQUIRE_FALSE(fix.loop().tick().has_value());
    CHECK(fix.world.alive(spawned.ref));
    // The runtime `at:` placement mirrors FIRST (council fix G6: every
    // native-Transform seed on a script-component-hosting entity reaches
    // the script directory), then base component on the root, then the
    // state component on Idle — all routed through the ONE dispatcher, in
    // materialization order.
    CHECK(materializer.log ==
          std::vector<std::string>{
              "mirror", "base:Pulse@" + std::to_string(spawned.ref.index), "state:Aura@Idle"});
    // The deferred initial entry STARTED (D2's split, run by realize):
    // the machine is live in its initial state and entity.spawned fired.
    const EntityRef machine_root = fix.hierarchy.first_child_of(spawned.ref);
    const auto* root_component = fix.world.try_get<statechart::MachineRoot>(machine_root);
    REQUIRE(root_component != nullptr);
    CHECK(fix.chart().in_state(root_component->machine, Name("main"), Name("Idle")));
    CHECK(bus_log == std::vector<std::string>{"g:entity.spawned"});

    // And the fail-closed default: the SAME prefab through a spawner with
    // no materializer wired refuses at realize — never a silent omit.
    ChartFixture bare_fix;
    EventsDecl bare_events;
    PrefabSpawner bare(bare_fix.world,
                       bare_fix.hierarchy,
                       bare_fix.chart(),
                       bare_fix.bus(),
                       bare_fix.writer(),
                       bare_fix.registry,
                       bare_events,
                       vocab,
                       &bare_fix.loop());
    bare_fix.loop().set_structural_realizer(
        [&](std::uint64_t phase_record_id) { return bare.realize(phase_record_id); });
    const std::string bare_path = write_component_goblin(bare_fix.dir);
    PrefabSpawnResult bare_spawned = bare.spawn_prefab(bare_path, math::Vec3{});
    REQUIRE_FALSE(bare_spawned.error.has_value());
    std::optional<base::Error> halted = bare_fix.loop().tick();
    REQUIRE(halted.has_value());
    CHECK(unwrap(halted).code == "component.no_materializer");
}
