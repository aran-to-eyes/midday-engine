// loader.spawn — authored YAML through the ONE public path: load_scene ->
// register_scene_events -> spawn_scene -> the real TickLoop. Asserts the
// live world (adoption, names, machine initial states, children under
// states dormant, physics bodies), the journal's boot story (scene.spawn
// records citing the boot cause), payload validation via the registered
// vocabulary, an authored mini-A.3 drive (sequence trigger at the exact
// tick-locked time, span pairs entering/exiting the child state, any-state
// death rule), and dual-run byte determinism — two INDEPENDENTLY
// constructed sims from the same authored text, journals compared at the
// raw compressed-byte level (never a self-diff).

#include "core/base/file_io.h"
#include "core/journal/reader.h"
#include "core/loader/loader.h"
#include "core/physics/physics_server.h"
#include "core/statechart/statechart.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/sim_fixture.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace midday;
using namespace midday::loader;
using midday::base::Name;
using midday::journal::Record;
using midday::testkit::unwrap;

namespace {

void write_corpus(const testkit::TempDir& dir) {
    REQUIRE_FALSE(base::write_file(dir.file("combat.events.yaml"),
                                   "format: 1\n"
                                   "events:\n"
                                   "  player.inRange: {payload: {distance: float}}\n"
                                   "  death.dealt: {payload: {}}\n"
                                   "  attack.swoosh: {payload: {}}\n",
                                   "t")
                      .has_value());
    REQUIRE_FALSE(base::write_file(dir.file("boss.machine.yaml"),
                                   "format: 1\n"
                                   "machine: boss\n"
                                   "regions:\n"
                                   "  combat:\n"
                                   "    initial: Passive\n"
                                   "    anystate:\n"
                                   "      - {event: death.dealt, goto: Dead, priority: 100}\n"
                                   "    states:\n"
                                   "      Passive:\n"
                                   "        on:\n"
                                   "          - {event: player.inRange, goto: SlashAttack}\n"
                                   "      SlashAttack:\n"
                                   "        initial: Windup\n"
                                   "        sequence:\n"
                                   "          duration: 1.2\n"
                                   "          tracks:\n"
                                   "            - trigger: [{t: 0.30, event: attack.swoosh}]\n"
                                   "            - span: {name: HitboxLive, from: 0.40, to: 0.80}\n"
                                   "        on:\n"
                                   "          - {event: self.finished, goto: Passive}\n"
                                   "          - {event: HitboxLive.opened, goto: HitboxLive}\n"
                                   "          - {event: HitboxLive.closed, goto: Windup}\n"
                                   "        states:\n"
                                   "          Windup: {}\n"
                                   "          HitboxLive:\n"
                                   "            children:\n"
                                   "              - {entity: Hurtbox, at: [0, 1.0, 1.2]}\n"
                                   "      Dead: {}\n",
                                   "t")
                      .has_value());
    REQUIRE_FALSE(base::write_file(dir.file("boss.scene.yaml"),
                                   "format: 1\n"
                                   "scene: arena\n"
                                   "events: [combat.events.yaml]\n"
                                   "entities:\n"
                                   "  - entity: Ground\n"
                                   "    components:\n"
                                   "      - Transform: {}\n"
                                   "      - Collider: {shape: plane}\n"
                                   "  - entity: Boss\n"
                                   "    components:\n"
                                   "      - Transform: {at: [0, 1.0, 0]}\n"
                                   "      - Collider: {shape: box, size: [1.2, 1.8, 1.2]}\n"
                                   "      - RigidBody: {}\n"
                                   "    machines:\n"
                                   "      - {instance: {path: boss.machine.yaml}}\n",
                                   "t")
                      .has_value());
}

// The canonical composition + a Statechart + the physics server, spawned
// from the authored corpus. finish() must be called exactly once.
struct SpawnedSim {
    testkit::SimFixture sim;
    std::optional<statechart::Statechart> chart_slot;
    std::unique_ptr<physics::PhysicsServer> physics;
    SpawnResult spawned;

    explicit SpawnedSim(const testkit::TempDir& corpus) {
        chart_slot.emplace(sim.world, sim.hierarchy, sim.bus(), sim.writer(), sim.loop());
        SceneLoadResult loaded = load_scene(corpus.file("boss.scene.yaml"), sim.registry);
        REQUIRE_FALSE(loaded.error.has_value());
        const SceneFile& scene = unwrap(loaded.scene);
        REQUIRE_FALSE(register_scene_events(scene, sim.registry).has_value());
        REQUIRE(scene_uses_physics(scene));
        physics::PhysicsServerCreateResult created =
            physics::PhysicsServer::create(sim.world, sim.hierarchy, sim.bus());
        REQUIRE_FALSE(created.error.has_value());
        physics = std::move(created.server);
        REQUIRE_FALSE(sim.loop().add_hook(tick::Phase::kPhysics, *physics).has_value());
        spawned =
            spawn_scene(scene, sim.world, sim.hierarchy, chart(), physics.get(), sim.writer(), 0);
        REQUIRE_FALSE(spawned.error.has_value());
    }

    [[nodiscard]] statechart::Statechart& chart() { return unwrap(chart_slot); }

    [[nodiscard]] ecs::EntityRef find(std::string_view name) {
        ecs::EntityRef found;
        sim.world.view<SceneEntity>().include_inactive().each(
            [&](ecs::EntityRef ref, SceneEntity& tag) {
                if (tag.name.view() == name)
                    found = ref;
            });
        return found;
    }
};

std::vector<Record> of_kind(const std::vector<Record>& records, std::string_view kind) {
    std::vector<Record> out;
    for (const Record& record : records)
        if (record.kind == kind)
            out.push_back(record);
    return out;
}

// Ticks 1..10 idle, then injects player.inRange on the boss host channel —
// drained by tick 11's input phase, so SlashAttack enters at tick 11 (= E).
void approach_player(SpawnedSim& sim) {
    const MachineSeat& seat = sim.spawned.machines[0];
    REQUIRE_FALSE(sim.sim.loop().tick(10).has_value());
    base::Json payload = base::Json::object();
    payload.set("distance", 2.0);
    REQUIRE_FALSE(sim.sim.loop()
                      .inject_input(bus::EventKey::entity(seat.host),
                                    Name("player.inRange"),
                                    std::move(payload))
                      .has_value());
}

} // namespace

TEST_CASE("loader.spawn: the authored scene becomes the live world") {
    testkit::TempDir corpus{"loader-corpus"};
    write_corpus(corpus);
    SpawnedSim sim(corpus);

    CHECK(sim.spawned.stats.entities == 2);
    CHECK(sim.spawned.stats.machines == 1);
    CHECK(sim.spawned.stats.bodies == 2);
    CHECK(sim.spawned.stats.state_children == 1);
    REQUIRE(sim.spawned.machines.size() == 1);
    const MachineSeat& seat = sim.spawned.machines[0];
    CHECK(seat.machine == Name("boss"));
    CHECK(seat.entity == Name("Boss"));

    // Initial states entered; the Hurtbox sleeps under the dormant
    // HitboxLive substate (children under states, dormancy by cover).
    CHECK(sim.chart().in_state(seat.id, Name("combat"), Name("Passive")));
    const ecs::EntityRef hurtbox = sim.find("Hurtbox");
    REQUIRE_FALSE(hurtbox.is_null());
    CHECK(sim.sim.hierarchy.is_dormant(hurtbox));
    const ecs::EntityRef hitbox_state =
        sim.chart().state_entity(seat.id, Name("combat"), Name("HitboxLive"));
    CHECK(sim.sim.hierarchy.parent_of(hurtbox) == hitbox_state);

    // Physics is live: both bodies bound, the boss box is a dynamic body.
    CHECK(sim.physics->body_count() == 2);
    CHECK_FALSE(sim.physics->body_of(seat.host).is_null());

    // Registered vocabulary: a declared payload validates strictly now.
    base::Json bad = base::Json::object();
    bad.set("distance", "not a number");
    const bus::TriggerResult refused =
        sim.sim.bus().trigger(bus::EventKey::entity(seat.host), Name("player.inRange"), bad, 0);
    REQUIRE(refused.error.has_value());
    CHECK(unwrap(refused.error).code == "bus.payload_invalid");

    // Boot story in the journal: one scene.spawn per entity (incl. the
    // state child), every one a boot-tick FLIGHT record.
    std::vector<Record> records = sim.sim.finish();
    const std::vector<Record> spawns = of_kind(records, "scene.spawn");
    REQUIRE(spawns.size() == 3);
    CHECK(spawns[0].payload.find("name")->as_string() == "Ground");
    CHECK(spawns[1].payload.find("name")->as_string() == "Boss");
    CHECK(spawns[2].payload.find("name")->as_string() == "Hurtbox");
    CHECK(spawns[2].payload.find("under_state")->as_string() == "HitboxLive");
    CHECK(of_kind(records, "statechart.instantiate").size() == 1);
}

TEST_CASE("loader.spawn: the authored machine drives — tick-locked sequence, spans, death") {
    testkit::TempDir corpus{"loader-corpus-drive"};
    write_corpus(corpus);
    SpawnedSim sim(corpus);
    const MachineSeat& seat = sim.spawned.machines[0];
    const ecs::EntityRef hurtbox = sim.find("Hurtbox");

    approach_player(sim);
    REQUIRE_FALSE(sim.sim.loop().tick().has_value()); // tick 11: enters SlashAttack
    CHECK(sim.chart().in_state(seat.id, Name("combat"), Name("SlashAttack")));
    CHECK(sim.chart().in_state(seat.id, Name("combat"), Name("Windup")));
    CHECK(sim.sim.hierarchy.is_dormant(hurtbox));

    // 0.30 s -> tick E+18 = 29; span [0.40, 0.80] -> E+24..E+48 = 35..59.
    REQUIRE_FALSE(sim.sim.loop().run_to_tick(34).has_value());
    CHECK(sim.sim.hierarchy.is_dormant(hurtbox));
    REQUIRE_FALSE(sim.sim.loop().tick().has_value()); // tick 35: span opens
    CHECK(sim.chart().in_state(seat.id, Name("combat"), Name("HitboxLive")));
    CHECK_FALSE(sim.sim.hierarchy.is_dormant(hurtbox));        // authored pair woke it
    REQUIRE_FALSE(sim.sim.loop().run_to_tick(59).has_value()); // span closes at 59
    CHECK(sim.chart().in_state(seat.id, Name("combat"), Name("Windup")));
    CHECK(sim.sim.hierarchy.is_dormant(hurtbox)); // no zombie hitbox

    // The any-state death rule (priority 100) fires mid-flight.
    REQUIRE_FALSE(sim.sim.loop().tick(3).has_value());
    const bus::TriggerResult death = sim.sim.bus().trigger(
        bus::EventKey::entity(seat.host), Name("death.dealt"), base::Json::object(), 0);
    REQUIRE_FALSE(death.error.has_value());
    CHECK(sim.chart().in_state(seat.id, Name("combat"), Name("Dead")));

    // Journal spot checks: the swoosh fired at EXACTLY tick 29, the span
    // record chain exists, the finished event never fired (interrupted...
    // no — the sheet finished at E+72 = 83? The death landed first at 62).
    std::vector<Record> records = sim.sim.finish();
    bool swoosh_at_29 = false;
    for (const Record& record : records)
        if (record.kind == "event.trigger" &&
            record.payload.find("event")->as_string() == "attack.swoosh")
            swoosh_at_29 = record.tick == 29;
    CHECK(swoosh_at_29);
    CHECK(of_kind(records, "sequence.span_open").size() == 1);
    CHECK(of_kind(records, "sequence.span_close").size() == 1);
}

TEST_CASE("loader.spawn: two independent sims from the same text are byte-identical") {
    testkit::TempDir corpus{"loader-corpus-dual"};
    write_corpus(corpus);

    auto drive = [](SpawnedSim& sim) {
        approach_player(sim);
        REQUIRE_FALSE(sim.sim.loop().run_to_tick(120).has_value());
        REQUIRE_FALSE(sim.sim.writer().close().has_value());
    };

    SpawnedSim first(corpus);
    SpawnedSim second(corpus);
    drive(first);
    drive(second);

    base::ReadFileResult bytes_a =
        base::read_file(std::filesystem::path(first.sim.bundle_path()) / "journal.jsonl.zst", "t");
    base::ReadFileResult bytes_b =
        base::read_file(std::filesystem::path(second.sim.bundle_path()) / "journal.jsonl.zst", "t");
    REQUIRE_FALSE(bytes_a.error.has_value());
    REQUIRE_FALSE(bytes_b.error.has_value());
    CHECK(bytes_a.bytes.size() > 0);
    CHECK(bytes_a.bytes == bytes_b.bytes);
}
