// loader.despawn_journal — PrefabSpawner's despawn JOURNAL contract at the
// phase-8 prepare (M2 0B council fixes C1 + K6): "no record, no effect" — a
// poisoned writer refusing the prefab.despawn record aborts the despawn
// BEFORE any effect in BOTH prepare branches (alive: no exit chains, no
// hooks, no queued removal; pending: the record is written before the queue,
// never after) — and an accepted linger whose target died by another route
// is dropped with a FLIGHT "prefab.despawn_stale" breadcrumb, never
// silently. The linger mechanics themselves (ceiling arithmetic, deadline
// merge, cutoffs, phase-8 order) live in core/loader/despawn_linger_test.cpp.

#include "core/base/file_io.h"
#include "core/loader/prefab_spawn.h"
#include "core/loader/prefab_test_support.h"
#include "core/statechart/test_support.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

using namespace midday;
using midday::base::Json;
using midday::journal::Record;
using midday::loader::PrefabSpawnResult;
using midday::loader::test::despawned_triggers;
using midday::loader::test::EffectObservers;
using midday::loader::test::PrefabFixture;
using midday::loader::test::write_goblin_prefab;
using midday::statechart::test::field;
using midday::statechart::test::of_kind;
using midday::testkit::unwrap;

TEST_CASE("loader.despawn_journal: no record, no effect — a poisoned journal aborts the despawn "
          "BEFORE any effect (no exit chains, no base onExit, no reap, no entity.despawned)") {
    PrefabFixture pf;
    const std::string prefab_path = write_goblin_prefab(pf.fix.dir);
    PrefabSpawnResult spawned = pf.spawner.spawn_prefab(prefab_path, math::Vec3{});
    REQUIRE_FALSE(spawned.error.has_value());

    // The bus_journal_test poisoning idiom: citing a never-consumed cause is
    // journal.cause_unknown — the sticky error, record() returns 0 forever.
    auto poison = [&] {
        CHECK(pf.fix.writer().record(2, journal::Tier::Flight, "poison", 9999, Json::object()) ==
              0);
        REQUIRE(pf.fix.writer().status().has_value());
    };

    SUBCASE("alive branch: the record refuses before exit chains, hooks, or the queue") {
        REQUIRE_FALSE(pf.fix.loop().tick().has_value()); // tick 1: realized, alive
        REQUIRE(pf.fix.world.alive(spawned.ref));

        EffectObservers obs(pf, spawned.ref);

        REQUIRE_FALSE(pf.spawner.despawn(spawned.ref).has_value()); // due: next cutoff, tick 2
        poison();

        // Drive phase 8's pre-flush half directly: the loop's own tick
        // record would refuse first (tick.journal_refused) and never reach
        // the spawner — prepare() is the unit under test.
        std::optional<base::Error> refused = pf.spawner.prepare(2, 0);
        REQUIRE(refused.has_value());
        CHECK(unwrap(refused).code == "despawn.journal_refused");
        CHECK(unwrap(refused).details.find("journal") != nullptr);

        // NO effect executed: no exit chain, no despawn_exit, no queued
        // reap awaiting realize — and the flush finds no removal to apply.
        CHECK(obs.log.empty());
        CHECK(pf.spawner.pending_despawn_count() == 0);
        REQUIRE_FALSE(pf.fix.world.flush_structural().has_value());
        CHECK(pf.fix.world.alive(spawned.ref));
        REQUIRE_FALSE(pf.spawner.realize(0).has_value());
        CHECK(obs.log.empty()); // no note, no reap, no entity.despawned
        CHECK(pf.fix.world.alive(spawned.ref));
    }

    SUBCASE("pending branch: the record refuses BEFORE the queue — record-before-effect") {
        // Spawn queued, never flushed: the ref is PENDING, a legal despawn
        // target (queue order would resolve born-then-dead at the flush).
        REQUIRE_FALSE(pf.spawner.despawn(spawned.ref).has_value()); // due: cutoff tick 1
        poison();

        std::optional<base::Error> refused = pf.spawner.prepare(1, 0);
        REQUIRE(refused.has_value());
        CHECK(unwrap(refused).code == "despawn.journal_refused");
        CHECK(pf.spawner.pending_despawn_count() == 0); // nothing queued for realize

        // The flush applies the SPAWN only: had the despawn been queued
        // before the refused record (the old effect-before-record order),
        // queue order would have killed the entity right here.
        REQUIRE_FALSE(pf.fix.world.flush_structural().has_value());
        CHECK(pf.fix.world.alive(spawned.ref));
    }
}

TEST_CASE("loader.despawn_journal: an accepted linger whose target died by another route "
          "journals a prefab.despawn_stale breadcrumb at the due tick — never a silent drop") {
    PrefabFixture pf;
    const std::string prefab_path = write_goblin_prefab(pf.fix.dir);
    PrefabSpawnResult spawned = pf.spawner.spawn_prefab(prefab_path, math::Vec3{});
    REQUIRE_FALSE(spawned.error.has_value());
    REQUIRE_FALSE(pf.fix.loop().tick().has_value()); // tick 1: realized, alive
    REQUIRE(pf.fix.world.alive(spawned.ref));

    // Accept the linger (bus tick still 1): 0.05 * 60 is exactly 3.0 in
    // IEEE doubles -> due tick 1 + 3 = 4 ... then kill the target by
    // ANOTHER route — a raw ECS despawn, no prefab.despawn machinery —
    // applied by tick 2's flush.
    REQUIRE_FALSE(pf.spawner.despawn(spawned.ref, loader::DespawnOptions{0.05}).has_value());
    REQUIRE_FALSE(pf.fix.world.queue_despawn(spawned.ref).has_value());
    REQUIRE_FALSE(pf.fix.loop().tick().has_value()); // tick 2: the raw reap
    CHECK_FALSE(pf.fix.world.alive(spawned.ref));

    // The due tick: the stale arm drops the entry — WITH the breadcrumb.
    REQUIRE_FALSE(pf.fix.loop().run_to_tick(4).has_value());
    CHECK(pf.spawner.pending_despawn_count() == 0);

    std::vector<Record> records = pf.fix.finish();
    CHECK(of_kind(records, "prefab.despawn").empty()); // no despawn executed...
    CHECK(despawned_triggers(records).empty());        // ...and no event fired
    const std::vector<Record> stale = of_kind(records, "prefab.despawn_stale");
    REQUIRE(stale.size() == 1);
    CHECK(stale[0].tick == 4);
    CHECK(field(stale[0].payload, "entity").as_int() ==
          static_cast<std::int64_t>(spawned.ref.to_bits()));
    CHECK(field(stale[0].payload, "requested").as_int() == 1);
    CHECK(field(stale[0].payload, "due").as_int() == 4);
}
