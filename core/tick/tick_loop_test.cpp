// tick.hooks / tick.input / tick.structural — the loop's contracts beyond
// raw phase order: ordered hook registration with pinned refusal codes, the
// input injection seam (FIFO roots, next-tick cutoff, bad inputs never stop
// the heartbeat), and phase 8 as THE one deterministic mutation point
// (queued structure applies there and nowhere else; transforms propagate).

#include "core/base/json.h"
#include "core/base/name.h"
#include "core/bus/bus.h"
#include "core/ecs/world.h"
#include "core/hierarchy/hierarchy.h"
#include "core/journal/record.h"
#include "core/math/xform.h"
#include "core/tick/test_support.h"
#include "core/tick/tick_loop.h"
#include "testkit/doctest.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using midday::base::Json;
using midday::base::Name;
using midday::bus::EventKey;
using midday::ecs::EntityRef;
using midday::journal::Record;
using midday::math::Transform;
using midday::math::Vec3;
using midday::tick::Phase;
using midday::tick::PhaseContext;
using midday::tick::TickLoop;
using midday::tick::test::code_of;
using midday::tick::test::field;
using midday::tick::test::RecordingHook;
using midday::tick::test::TickFixture;

TEST_CASE("tick.hooks: registration order within a phase, phase order across phases") {
    TickFixture fix;
    std::vector<std::string> log;
    RecordingHook update_a("A", log);
    RecordingHook update_b("B", log);
    RecordingHook watcher("W", log);
    RecordingHook physics("P", log);
    // Registered out of phase order on purpose — execution follows the
    // PHASE order, then registration order inside each phase.
    REQUIRE_FALSE(fix.loop().add_hook(Phase::kPhysics, physics).has_value());
    REQUIRE_FALSE(fix.loop().add_hook(Phase::kUpdate, update_a).has_value());
    REQUIRE_FALSE(fix.loop().add_hook(Phase::kWatchers, watcher).has_value());
    REQUIRE_FALSE(fix.loop().add_hook(Phase::kUpdate, update_b).has_value());
    CHECK(fix.loop().hook_count(Phase::kUpdate) == 2);

    REQUIRE_FALSE(fix.loop().tick().has_value());
    const std::vector<std::string> expected = {
        "W@watchers:1", "A@update:1", "B@update:1", "P@physics:1"};
    CHECK(log == expected);

    // remove_hook between ticks: survivors keep their order.
    REQUIRE_FALSE(fix.loop().remove_hook(Phase::kUpdate, update_a).has_value());
    log.clear();
    REQUIRE_FALSE(fix.loop().tick().has_value());
    const std::vector<std::string> after = {"W@watchers:2", "B@update:2", "P@physics:2"};
    CHECK(log == after);
}

TEST_CASE("tick.hooks: the context hands each hook its phase marker as the cause anchor") {
    TickFixture fix;
    std::vector<std::string> log;
    RecordingHook hook("H", log);
    PhaseContext seen;
    hook.action = [&seen](TickLoop&, const PhaseContext& context) { seen = context; };
    REQUIRE_FALSE(fix.loop().add_hook(Phase::kSequences, hook).has_value());
    REQUIRE_FALSE(fix.loop().tick().has_value());

    CHECK(seen.phase == Phase::kSequences);
    CHECK(seen.tick == 1);
    CHECK(seen.dt == fix.loop().dt_seconds());

    std::vector<Record> records = fix.finish();
    // The context's ids point at real journal records of the right shape.
    REQUIRE(seen.phase_record_id >= 1);
    const Record& marker = records[seen.phase_record_id - 1];
    CHECK(marker.kind == "tick.phase");
    CHECK(field(marker.payload, "phase").as_string() == "sequences");
    const Record& root = records[seen.tick_record_id - 1];
    CHECK(field(root.payload, "phase").as_string() == "tick-begin");
    CHECK(marker.cause_id == root.id);
}

TEST_CASE("tick.hooks: refusal codes — reserved phases, duplicates, absent removes") {
    TickFixture fix;
    std::vector<std::string> log;
    RecordingHook hook("H", log);
    CHECK(code_of(fix.loop().add_hook(Phase::kTickBegin, hook)) == "tick.phase_reserved");
    CHECK(code_of(fix.loop().add_hook(Phase::kInput, hook)) == "tick.phase_reserved");
    CHECK(code_of(fix.loop().add_hook(Phase::kStructuralApply, hook)) == "tick.phase_reserved");
    CHECK(code_of(fix.loop().add_hook(Phase::kTickEnd, hook)) == "tick.phase_reserved");
    REQUIRE_FALSE(fix.loop().add_hook(Phase::kUpdate, hook).has_value());
    CHECK(code_of(fix.loop().add_hook(Phase::kUpdate, hook)) == "tick.duplicate_hook");
    CHECK(code_of(fix.loop().remove_hook(Phase::kPost, hook)) == "tick.hook_absent");
    CHECK(fix.loop().hook_count(Phase::kTickBegin) == 0);
}

TEST_CASE("tick.hooks: mid-tick wiring is locked, re-entrant tick() refuses") {
    TickFixture fix;
    std::vector<std::string> log;
    RecordingHook hook("H", log);
    RecordingHook other("O", log);
    std::string add_code;
    std::string remove_code;
    std::string tick_code;
    hook.action = [&](TickLoop& loop, const PhaseContext&) {
        add_code = code_of(loop.add_hook(Phase::kPost, other));
        remove_code = code_of(loop.remove_hook(Phase::kUpdate, hook));
        tick_code = code_of(loop.tick());
        CHECK(loop.ticking());
    };
    REQUIRE_FALSE(fix.loop().add_hook(Phase::kUpdate, hook).has_value());
    REQUIRE_FALSE(fix.loop().tick().has_value()); // the OUTER tick still completes
    CHECK(add_code == "tick.hook_locked");
    CHECK(remove_code == "tick.hook_locked");
    CHECK(tick_code == "tick.reentrant");
    CHECK(fix.loop().current_tick() == 1);
    CHECK_FALSE(fix.loop().ticking());
}

TEST_CASE("tick.input: FIFO drain as root records; mid-tick injections deliver NEXT tick") {
    TickFixture fix;
    const EventKey key = EventKey::named(Name("global"));

    // A hook that injects a new input DURING tick 1 — after the cutoff.
    std::vector<std::string> log;
    RecordingHook hook("H", log);
    hook.action = [&key](TickLoop& loop, const PhaseContext& context) {
        if (context.tick != 1)
            return;
        Json payload = Json::object();
        payload.set("n", 3);
        REQUIRE_FALSE(loop.inject_input(key, Name("late.custom"), payload).has_value());
    };
    REQUIRE_FALSE(fix.loop().add_hook(Phase::kUpdate, hook).has_value());

    Json first = Json::object();
    first.set("n", 1);
    Json second = Json::object();
    second.set("n", 2);
    REQUIRE_FALSE(fix.loop().inject_input(key, Name("a.custom"), first).has_value());
    REQUIRE_FALSE(fix.loop().inject_input(key, Name("b.custom"), second).has_value());
    CHECK(fix.loop().pending_input_count() == 2);
    REQUIRE_FALSE(fix.loop().tick().has_value());
    CHECK(fix.loop().pending_input_count() == 1); // the late injection waits
    REQUIRE_FALSE(fix.loop().tick().has_value());
    CHECK(fix.loop().pending_input_count() == 0);
    CHECK(fix.loop().stats().inputs_injected == 3);
    CHECK(fix.loop().stats().inputs_delivered == 3);

    std::vector<Record> records = fix.finish();
    std::vector<std::pair<std::uint64_t, std::string>> triggers;
    for (const Record& record : records) {
        if (record.kind != "event.trigger")
            continue;
        CHECK(record.cause_id == 0); // every injected input is a ROOT record
        triggers.emplace_back(record.tick, field(record.payload, "event").as_string());
    }
    const std::vector<std::pair<std::uint64_t, std::string>> expected = {
        {1, "a.custom"}, {1, "b.custom"}, {2, "late.custom"}};
    CHECK(triggers == expected);
}

TEST_CASE("tick.input: a refused input is counted and skipped — the heartbeat goes on") {
    TickFixture fix;
    const EventKey key = EventKey::named(Name("global"));
    Json bad = Json::object();
    bad.set("action", "jump"); // action.pressed: strength + device missing
    Json good = Json::object();
    good.set("n", 1);
    REQUIRE_FALSE(fix.loop().inject_input(key, Name("action.pressed"), bad).has_value());
    REQUIRE_FALSE(fix.loop().inject_input(key, Name("ok.custom"), good).has_value());
    REQUIRE_FALSE(fix.loop().tick().has_value()); // no error surfaces
    CHECK(fix.loop().stats().inputs_refused == 1);
    CHECK(fix.loop().stats().inputs_delivered == 1);

    // The journal explains itself: the refusal record sits where the input
    // would have, and the good input still delivered.
    std::vector<Record> records = fix.finish();
    CHECK(records[2].kind == "bus.payload_invalid");
    CHECK(records[3].kind == "event.trigger");
    CHECK(field(records[3].payload, "event").as_string() == "ok.custom");
}

TEST_CASE("tick.input: the null key is refused at injection time") {
    TickFixture fix;
    CHECK(code_of(fix.loop().inject_input(EventKey(), Name("x"), Json::object())) ==
          "tick.null_key");
    CHECK(fix.loop().stats().inputs_injected == 0);
}

TEST_CASE("tick.structural: queued structure applies at phase 8 — not before, not after") {
    TickFixture fix;
    midday::ecs::World& world = fix.world;
    midday::hierarchy::Hierarchy& hierarchy = fix.hierarchy;

    // Boot: a parent and a child, adopted, with local transforms.
    const EntityRef parent = world.spawn();
    const EntityRef child = world.spawn();
    REQUIRE_FALSE(hierarchy.adopt(parent).has_value());
    REQUIRE_FALSE(hierarchy.adopt(child).has_value());
    Transform parent_local = Transform::identity();
    parent_local.translation = Vec3{1, 2, 3};
    Transform child_local = Transform::identity();
    child_local.translation = Vec3{10, 0, 0};
    REQUIRE_FALSE(hierarchy.set_local(parent, parent_local).has_value());
    REQUIRE_FALSE(hierarchy.set_local(child, child_local).has_value());

    // Queue: attach child under parent, spawn a third entity, despawn later.
    REQUIRE_FALSE(hierarchy.queue_attach(child, parent).has_value());
    const EntityRef spawned = world.queue_spawn();

    // Observe from the POST phase (7): the queue must still be pending.
    std::vector<std::string> log;
    RecordingHook post_probe("Q", log);
    bool pending_at_post = false;
    post_probe.action = [&](TickLoop&, const PhaseContext& context) {
        if (context.tick != 1)
            return;
        pending_at_post = !world.alive(spawned) && hierarchy.parent_of(child) != parent;
    };
    REQUIRE_FALSE(fix.loop().add_hook(Phase::kPost, post_probe).has_value());

    REQUIRE_FALSE(fix.loop().tick().has_value());
    CHECK(pending_at_post);      // phase 7 saw the old world
    CHECK(world.alive(spawned)); // phase 8 applied the spawn
    CHECK(hierarchy.parent_of(child) == parent);

    // And transforms propagated: the child's world translation composed.
    const auto* child_world = hierarchy.world_of(child);
    REQUIRE(child_world != nullptr);
    CHECK(child_world->element(0, 3) == doctest::Approx(11.0));
    CHECK(child_world->element(1, 3) == doctest::Approx(2.0));
    CHECK(child_world->element(2, 3) == doctest::Approx(3.0));

    // Despawn queues the same way and applies at the NEXT phase 8.
    REQUIRE_FALSE(world.queue_despawn(spawned).has_value());
    CHECK(world.alive(spawned));
    REQUIRE_FALSE(fix.loop().tick().has_value());
    CHECK_FALSE(world.alive(spawned));
}

TEST_CASE("tick.structural: the two-phase extension brackets the flush — prepare sees the old "
          "world, realize the new (M2 0B D4)") {
    TickFixture fix;

    // A queued spawn is the flush probe: pending BEFORE phase 8's flush,
    // alive after. prepare must observe the former, realize the latter —
    // exit-chains-before-removal at the exact ceiling tick depends on
    // exactly this bracketing (tick_loop.h).
    const EntityRef pending = fix.world.queue_spawn();

    std::vector<std::string> log;
    std::uint64_t prepare_phase_record = 0;
    std::uint64_t realize_phase_record = 0;
    fix.loop().set_structural_preparer(
        [&](std::uint64_t tick,
            std::uint64_t phase_record_id) -> std::optional<midday::base::Error> {
            prepare_phase_record = phase_record_id;
            log.push_back("prepare:t" + std::to_string(tick) +
                          (fix.world.alive(pending) ? ":alive" : ":pending"));
            return std::nullopt;
        });
    fix.loop().set_structural_realizer(
        [&](std::uint64_t phase_record_id) -> std::optional<midday::base::Error> {
            realize_phase_record = phase_record_id;
            log.push_back(std::string("realize") +
                          (fix.world.alive(pending) ? ":alive" : ":pending"));
            return std::nullopt;
        });

    REQUIRE_FALSE(fix.loop().tick().has_value());
    CHECK(log == std::vector<std::string>{"prepare:t1:pending", "realize:alive"});
    // Both halves ride the SAME structural-apply phase marker (the cause id
    // convention) — one phase, one record, two calls.
    CHECK(prepare_phase_record != 0);
    CHECK(prepare_phase_record == realize_phase_record);

    // A preparer error halts the tick exactly like any other phase failure,
    // BEFORE the flush mutates anything.
    const EntityRef second = fix.world.queue_spawn();
    fix.loop().set_structural_preparer(
        [](std::uint64_t, std::uint64_t) -> std::optional<midday::base::Error> {
            return midday::base::Error{
                "test.prepare_stop", "halt before the flush", midday::base::Json::object()};
        });
    auto error = fix.loop().tick();
    REQUIRE(error.has_value());
    CHECK(midday::tick::test::unwrap(error).code == "test.prepare_stop");
    CHECK_FALSE(fix.world.alive(second)); // the flush never ran

    // Clearing the slot restores the pre-0B single-phase behavior.
    fix.loop().set_structural_preparer({});
    REQUIRE_FALSE(fix.loop().tick().has_value());
    CHECK(fix.world.alive(second));
}
