// loader.despawn_linger — PrefabSpawner's despawn-linger queue riding the
// two-phase structural extension (M2 0B track D, FUSED-SPEC D4): ceiling
// arithmetic to the EXACT due tick, the refusal matrix, earliest-deadline-
// wins double-despawn, phase-8 re-entrancy cutoffs, and the exact phase-8
// order (exit chains -> base onExit -> flush -> note(REAP tick) -> reap ->
// entity.despawned — the 0A G2 closure). The spawn-side G1 refusal lives
// with the other spawn_prefab tests (core/loader/prefab_spawn_test.cpp);
// the despawn JOURNAL contract (record-refusal abort, the stale-linger
// breadcrumb) lives in core/loader/despawn_journal_test.cpp.

#include "core/base/file_io.h"
#include "core/bus/test_support.h"
#include "core/loader/prefab_spawn.h"
#include "core/loader/prefab_test_support.h"
#include "core/statechart/test_support.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"

#include <cstdint>
#include <limits>
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
using midday::loader::test::despawned_triggers;
using midday::loader::test::EffectObservers;
using midday::loader::test::PrefabFixture;
using midday::loader::test::RecordingDespawnHooks;
using midday::loader::test::write_goblin_prefab;
using midday::statechart::test::field;
using midday::statechart::test::of_kind;
using midday::testkit::unwrap;

namespace {

// Fires despawn requests at scheduled ticks from an OPEN phase (kUpdate,
// phase 5) — genuinely mid-tick, phases 1-7 territory, before the tick's
// own phase-8 cutoff.
struct DespawnAtTick final : tick::PhaseHook {
    PrefabSpawner* spawner = nullptr;

    struct Request {
        std::uint64_t tick = 0;
        EntityRef ref;
        loader::DespawnOptions opts;
    };

    std::vector<Request> requests;

    void on_phase(tick::TickLoop&, const tick::PhaseContext& context) override {
        for (const Request& request : requests) {
            if (request.tick != context.tick)
                continue;
            REQUIRE_FALSE(spawner->despawn(request.ref, request.opts).has_value());
        }
    }
};

// Observes aliveness from the POST phase (7): the linger contract says a
// corpse is FULLY alive through phases 1-7 of its due tick.
struct AliveAtPost final : tick::PhaseHook {
    ecs::World* world = nullptr;
    EntityRef ref;
    std::vector<std::uint64_t> alive_ticks;

    void on_phase(tick::TickLoop&, const tick::PhaseContext& context) override {
        if (world->alive(ref))
            alive_ticks.push_back(context.tick);
    }
};

} // namespace

TEST_CASE("loader.despawn_linger: ceiling arithmetic lands the reap on the EXACT due tick — "
          "{after: 4.0}@60 from tick 1 despawns at 241, {after: 0.025} ceilings to +2") {
    PrefabFixture pf;
    const std::string prefab_path = write_goblin_prefab(pf.fix.dir);

    // Two goblins, requested pre-tick: both realize at tick 1's phase 8.
    PrefabSpawnResult g1 = pf.spawner.spawn_prefab(prefab_path, math::Vec3{});
    PrefabSpawnResult g2 = pf.spawner.spawn_prefab(prefab_path, math::Vec3{});
    REQUIRE_FALSE(g1.error.has_value());
    REQUIRE_FALSE(g2.error.has_value());

    // g1: despawned at tick 1 (mid-tick, while its own spawn is still
    // pending — a legal target) with the golden's exact linger:
    //   1 + ceil(4.0 * 60) = 241  (the D6 arithmetic, pinned literally).
    // g2: despawned at tick 2 with a NON-integral product:
    //   2 + ceil(0.025 * 60 = 1.500..) = 4  (floor would land 3 — the
    //   off-by-one falsifier's red line).
    DespawnAtTick killer;
    killer.spawner = &pf.spawner;
    killer.requests.emplace_back(1, g1.ref, loader::DespawnOptions{4.0});
    killer.requests.emplace_back(2, g2.ref, loader::DespawnOptions{0.025});
    REQUIRE_FALSE(pf.fix.loop().add_hook(tick::Phase::kUpdate, killer).has_value());

    AliveAtPost probe1;
    probe1.world = &pf.fix.world;
    probe1.ref = g1.ref;
    AliveAtPost probe2;
    probe2.world = &pf.fix.world;
    probe2.ref = g2.ref;
    REQUIRE_FALSE(pf.fix.loop().add_hook(tick::Phase::kPost, probe1).has_value());
    REQUIRE_FALSE(pf.fix.loop().add_hook(tick::Phase::kPost, probe2).has_value());

    // g2 first: alive through phase 7 of tick 4 (the corpse rule), dead
    // once tick 4's phase 8 ran, NOT dead a tick early (ceil, not floor).
    REQUIRE_FALSE(pf.fix.loop().run_to_tick(3).has_value());
    CHECK(pf.fix.world.alive(g2.ref));
    REQUIRE_FALSE(pf.fix.loop().tick().has_value()); // tick 4
    CHECK_FALSE(pf.fix.world.alive(g2.ref));
    CHECK(probe2.alive_ticks.back() == 4); // phases 1-7 of the due tick saw it

    // g1: alive through 240 full ticks, dead exactly at 241.
    REQUIRE_FALSE(pf.fix.loop().run_to_tick(240).has_value());
    CHECK(pf.fix.world.alive(g1.ref));
    REQUIRE_FALSE(pf.fix.loop().tick().has_value()); // tick 241
    CHECK_FALSE(pf.fix.world.alive(g1.ref));
    CHECK(probe1.alive_ticks.back() == 241);

    // Journal: prefab.despawn carries request/due; entity.despawned lands
    // AT the due tick, citing its despawn record.
    std::vector<Record> records = pf.fix.finish();
    const std::vector<Record> despawns = of_kind(records, "prefab.despawn");
    REQUIRE(despawns.size() == 2);
    CHECK(field(despawns[0].payload, "requested").as_int() == 2);
    CHECK(field(despawns[0].payload, "due").as_int() == 4);
    CHECK(despawns[0].tick == 4);
    CHECK(field(despawns[1].payload, "requested").as_int() == 1);
    CHECK(field(despawns[1].payload, "due").as_int() == 241);
    CHECK(despawns[1].tick == 241);
    const std::vector<Record> events = despawned_triggers(records);
    REQUIRE(events.size() == 2);
    CHECK(events[0].tick == 4);
    CHECK(events[0].cause_id == despawns[0].id);
    CHECK(events[1].tick == 241);
    CHECK(events[1].cause_id == despawns[1].id);
}

TEST_CASE("loader.despawn_linger: refusal matrix — bad after values, missing tick source, "
          "stale already-reaped ref") {
    PrefabFixture pf;
    const std::string prefab_path = write_goblin_prefab(pf.fix.dir);
    PrefabSpawnResult spawned = pf.spawner.spawn_prefab(prefab_path, math::Vec3{});
    REQUIRE_FALSE(spawned.error.has_value());
    REQUIRE_FALSE(pf.fix.loop().tick().has_value());
    REQUIRE(pf.fix.world.alive(spawned.ref));

    auto refuse = [&](double after) {
        std::optional<base::Error> error =
            pf.spawner.despawn(spawned.ref, loader::DespawnOptions{after});
        REQUIRE(error.has_value());
        CHECK(unwrap(error).code == "despawn.bad_after");
        return unwrap(error).details.find("reason")->as_string();
    };
    CHECK(refuse(-1.0) == "negative");
    CHECK(refuse(std::numeric_limits<double>::quiet_NaN()) == "non_finite");
    CHECK(refuse(std::numeric_limits<double>::infinity()) == "non_finite");
    CHECK(refuse(1.0e300) == "overflow");
    CHECK(pf.spawner.pending_despawn_count() == 0); // nothing was recorded

    // A spawner with no TickLoop wired (every pre-0B composition) refuses a
    // linger fail-closed — never a silently-guessed tick rate. The
    // immediate path (after == 0) needs no rate and still works.
    EventsDecl bare_events;
    PrefabSpawner bare(pf.fix.world,
                       pf.fix.hierarchy,
                       pf.fix.chart(),
                       pf.fix.bus(),
                       pf.fix.writer(),
                       pf.fix.registry,
                       bare_events);
    std::optional<base::Error> no_source = bare.despawn(spawned.ref, loader::DespawnOptions{1.0});
    REQUIRE(no_source.has_value());
    CHECK(unwrap(no_source).code == "despawn.no_tick_source");
    REQUIRE_FALSE(bare.despawn(spawned.ref).has_value());
    CHECK(bare.pending_despawn_count() == 1);

    // Reap it through the wired spawner, then: a genuinely stale ref
    // refuses at the REQUEST site (D4's stale rule).
    REQUIRE_FALSE(pf.spawner.despawn(spawned.ref).has_value());
    REQUIRE_FALSE(pf.fix.loop().tick().has_value());
    REQUIRE_FALSE(pf.fix.world.alive(spawned.ref));
    std::optional<base::Error> stale = pf.spawner.despawn(spawned.ref);
    REQUIRE(stale.has_value());
    CHECK(unwrap(stale).code == "ecs.stale_handle");
    std::optional<base::Error> stale_linger =
        pf.spawner.despawn(spawned.ref, loader::DespawnOptions{2.0});
    REQUIRE(stale_linger.has_value());
    CHECK(unwrap(stale_linger).code == "ecs.stale_handle");
}

TEST_CASE("loader.despawn_linger: double-despawn is earliest-deadline-wins — immediate "
          "advances, later never postpones, repeats are idempotent") {
    PrefabFixture pf;
    const std::string prefab_path = write_goblin_prefab(pf.fix.dir);
    PrefabSpawnResult a = pf.spawner.spawn_prefab(prefab_path, math::Vec3{});
    PrefabSpawnResult b = pf.spawner.spawn_prefab(prefab_path, math::Vec3{});
    PrefabSpawnResult c = pf.spawner.spawn_prefab(prefab_path, math::Vec3{});
    REQUIRE_FALSE(pf.fix.loop().tick().has_value()); // all alive at tick 1

    // a: linger far out, then IMMEDIATE — the deadline advances to the
    // next cutoff (tick 2).
    REQUIRE_FALSE(pf.spawner.despawn(a.ref, loader::DespawnOptions{10.0}).has_value());
    REQUIRE_FALSE(pf.spawner.despawn(a.ref).has_value());
    // b: immediate, then a linger — which may never postpone it.
    REQUIRE_FALSE(pf.spawner.despawn(b.ref).has_value());
    REQUIRE_FALSE(pf.spawner.despawn(b.ref, loader::DespawnOptions{10.0}).has_value());
    // c: the same linger twice — idempotent (one queue entry, one record).
    REQUIRE_FALSE(pf.spawner.despawn(c.ref, loader::DespawnOptions{0.05}).has_value());
    REQUIRE_FALSE(pf.spawner.despawn(c.ref, loader::DespawnOptions{0.05}).has_value());
    CHECK(pf.spawner.pending_despawn_count() == 3); // one entry per entity

    REQUIRE_FALSE(pf.fix.loop().tick().has_value()); // tick 2
    CHECK_FALSE(pf.fix.world.alive(a.ref));
    CHECK_FALSE(pf.fix.world.alive(b.ref));
    CHECK(pf.fix.world.alive(c.ref)); // still lingering: 1 + ceil(0.05 * 60 = 3.0) = 4
    REQUIRE_FALSE(pf.fix.loop().run_to_tick(5).has_value());
    CHECK_FALSE(pf.fix.world.alive(c.ref));

    std::vector<Record> records = pf.fix.finish();
    CHECK(of_kind(records, "prefab.despawn").size() == 3); // exactly one per entity
    CHECK(despawned_triggers(records).size() == 3);
}

TEST_CASE("loader.despawn_linger: phase-8 re-entrancy — spawns and despawns requested by "
          "phase-8 listeners land NEXT tick, never in the just-finished flush") {
    PrefabFixture pf;
    const std::string prefab_path = write_goblin_prefab(pf.fix.dir);
    PrefabSpawnResult g1 = pf.spawner.spawn_prefab(prefab_path, math::Vec3{});
    PrefabSpawnResult g2 = pf.spawner.spawn_prefab(prefab_path, math::Vec3{});
    REQUIRE_FALSE(pf.fix.loop().tick().has_value()); // tick 1: both alive

    // g1's entity.despawned listener (phase 8, inside realize) despawns g2
    // immediately — the cutoff rule must land it at tick 3, not tick 2 —
    // and spawns a THIRD goblin, which must also realize at tick 3.
    std::vector<std::string> log;
    RecordingListener reaper("g1", log);
    EntityRef g3;
    reaper.action = [&](bus::Bus&, const bus::EventView& event) {
        if (event.event != Name("entity.despawned"))
            return;
        REQUIRE_FALSE(pf.spawner.despawn(g2.ref).has_value());
        PrefabSpawnResult late = pf.spawner.spawn_prefab(prefab_path, math::Vec3{});
        REQUIRE_FALSE(late.error.has_value());
        g3 = late.ref;
    };
    REQUIRE_FALSE(pf.fix.bus().subscribe(reaper, bus::EventKey::entity(g1.ref)).has_value());

    REQUIRE_FALSE(pf.spawner.despawn(g1.ref).has_value());
    REQUIRE_FALSE(pf.fix.loop().tick().has_value()); // tick 2: g1 reaps, listener fires
    CHECK_FALSE(pf.fix.world.alive(g1.ref));
    CHECK(pf.fix.world.alive(g2.ref)); // NOT this flush — next tick
    REQUIRE_FALSE(g3.is_null());
    CHECK_FALSE(pf.fix.world.alive(g3)); // reserved, realizes next tick

    REQUIRE_FALSE(pf.fix.loop().tick().has_value()); // tick 3
    CHECK_FALSE(pf.fix.world.alive(g2.ref));
    CHECK(pf.fix.world.alive(g3));
}

TEST_CASE("loader.despawn_linger: the exact phase-8 order — exit chains, base onExit, flush, "
          "note(REAP tick), reap, entity.despawned (G2 closed)") {
    PrefabFixture pf;
    const std::string prefab_path = write_goblin_prefab(pf.fix.dir);
    PrefabSpawnResult spawned = pf.spawner.spawn_prefab(prefab_path, math::Vec3{});
    REQUIRE_FALSE(pf.fix.loop().tick().has_value());
    REQUIRE(pf.fix.world.alive(spawned.ref));

    // Seat recording script hooks on the goblin's one state (the chart exit
    // chain surface) and wire the DespawnHooks double (the host seam).
    EffectObservers obs(pf, spawned.ref);
    std::vector<std::string>& log = obs.log;
    RecordingDespawnHooks& hooks = obs.hooks;

    // Requested between ticks (bus tick still 1): 0.05 * 60 rounds to
    // exactly 3.0 in IEEE doubles, ceil(3.0) = 3 → due tick 1 + 3 = 4.
    REQUIRE_FALSE(pf.spawner.despawn(spawned.ref, loader::DespawnOptions{0.05}).has_value());
    REQUIRE_FALSE(pf.fix.loop().run_to_tick(3).has_value());
    CHECK(pf.fix.world.alive(spawned.ref));
    REQUIRE_FALSE(pf.fix.loop().tick().has_value()); // tick 4: the due tick
    CHECK_FALSE(pf.fix.world.alive(spawned.ref));

    // The D4 order, across all three surfaces in ONE shared log: the chart
    // exit chain first, then base onExit (despawn_exit), then — post-flush
    // — note(REAP tick) and reap per subtree entity (children first, root
    // last), then the entity.despawned event.
    REQUIRE_FALSE(log.empty());
    CHECK(log.front() == "exit:Idle");
    auto index_of = [&](const std::string& prefix) {
        for (std::size_t i = 0; i < log.size(); ++i)
            if (log[i].starts_with(prefix))
                return i;
        return log.size();
    };
    const std::size_t first_exit = index_of("exit:");
    const std::size_t first_despawn_exit = index_of("despawn_exit:");
    const std::size_t first_note = index_of("note:");
    const std::size_t first_reap = index_of("reap:");
    const std::size_t despawned_event = index_of("bus:entity.despawned");
    REQUIRE(first_exit < log.size());
    REQUIRE(first_despawn_exit < log.size());
    REQUIRE(first_note < log.size());
    REQUIRE(first_reap < log.size());
    REQUIRE(despawned_event < log.size());
    CHECK(first_exit < first_despawn_exit);
    CHECK(first_despawn_exit < first_note);
    CHECK(first_note < first_reap);
    CHECK(first_reap < despawned_event);
    CHECK(despawned_event == log.size() - 1);

    // G2 CLOSED: every note_despawn carried the actual REAP tick — the due
    // tick, the tick entity.despawned journals at.
    REQUIRE_FALSE(hooks.note_ticks.empty());
    for (const std::uint64_t note_tick : hooks.note_ticks)
        CHECK(note_tick == 4);
    // The whole live subtree was reaped: root + machine root + region +
    // state — one (note, reap) pair per entity, deepest first, the ROOT's
    // pair LAST (reverse pre-order), right before the despawned event.
    CHECK(hooks.note_ticks.size() == 4);
    REQUIRE(log.size() >= 3);
    CHECK(log.at(log.size() - 3) == "note:" + std::to_string(spawned.ref.index) + "@4");
    CHECK(log.at(log.size() - 2) == "reap:" + std::to_string(spawned.ref.index));

    // Journal order mirrors the log: prefab.despawn -> the exit hook
    // record (citing it) -> entity.despawned (citing it), all at tick 5.
    std::vector<Record> records = pf.fix.finish();
    const std::vector<Record> despawns = of_kind(records, "prefab.despawn");
    REQUIRE(despawns.size() == 1);
    CHECK(despawns[0].tick == 4);
    std::vector<Record> exit_hooks;
    for (const Record& record : of_kind(records, "statechart.hook"))
        if (record.cause_id == despawns[0].id)
            exit_hooks.push_back(record);
    REQUIRE(exit_hooks.size() == 1);
    CHECK(field(exit_hooks[0].payload, "hook").as_string() == "exit");
    CHECK(exit_hooks[0].payload.find("peer") == nullptr);
    CHECK(exit_hooks[0].id > despawns[0].id);
    const std::vector<Record> events = despawned_triggers(records);
    REQUIRE(events.size() == 1);
    CHECK(events[0].cause_id == despawns[0].id);
    CHECK(events[0].id > exit_hooks[0].id);
}
