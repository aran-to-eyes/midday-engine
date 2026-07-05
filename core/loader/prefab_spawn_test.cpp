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

#include "core/bus/test_support.h"
#include "core/loader/prefab_spawn.h"
#include "core/loader/prefab_test_support.h"
#include "core/statechart/test_support.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"

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

// Fixture: ChartFixture (World+Hierarchy+Bus+Writer+TickLoop+Statechart)
// wired to a PrefabSpawner installed as the tick's structural realizer —
// the EXACT composition cli/verbs/run.cpp's RunSim would build for a real
// script host, minus the TS runtime itself (the engine mechanics are what
// the exit tests are about; ts/runtime/world_host_test.cpp covers the
// script-boundary plumbing separately).
struct PrefabFixture {
    ChartFixture fix;
    EventsDecl events;
    PrefabSpawner spawner;

    PrefabFixture()
        : spawner(fix.world,
                  fix.hierarchy,
                  fix.chart(),
                  fix.bus(),
                  fix.writer(),
                  fix.registry,
                  events) {
        fix.loop().set_structural_realizer(
            [this](std::uint64_t phase_record_id) { return spawner.realize(phase_record_id); });
    }
};

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
