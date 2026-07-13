// script.despawn_lifecycle — the FULL despawn-linger path with the REAL
// ComponentInstanceHost (M2 0B track D, FUSED-SPEC D4 + the 0A G2 closure):
// a runtime-spawned prefab carrying a TS base component and a TS state
// component, killed through PrefabSpawner's linger queue riding the
// two-phase structural extension. Pinned end to end:
//   * the phase-8 journal order — prefab.despawn -> the chart's
//     component_exit hook (state seat, exit-3) -> component.despawn_exit
//     (base seat, reverse attach) -> entity.despawned, all at the due tick;
//   * onExit hooks observe "" as the target state (there is none);
//   * reap silences delivery: an event at the dead entity's key reaches no
//     seat (no new component.on_event records);
//   * G2 observable from SCRIPT: after the reap, the stale-ref message
//     names the actual reap tick ("despawned at tick N"), never null.

#include "core/loader/loader.h"
#include "core/loader/prefab_spawn.h"
#include "ts/runtime/component_instance_test_support.h"

#include <string>
#include <vector>

using namespace midday;
using midday::base::Json;
using midday::base::Name;
using midday::journal::Record;
using midday::script::test::HostFixture;
using midday::script::test::manifest_entry;
using midday::script::test::triggers_of;
using midday::statechart::test::field;
using midday::statechart::test::of_kind;
using midday::testkit::unwrap;

namespace {

// First matching record's position in the whole journal, or records.size().
std::size_t index_of(const std::vector<Record>& records, std::uint64_t id) {
    for (std::size_t i = 0; i < records.size(); ++i)
        if (records[i].id == id)
            return i;
    return records.size();
}

// The production composition, shared by every case here: vocab from the SAME
// manifest the host loaded, spawner wired with the tick source, the host as
// BOTH the materializer and the DespawnHooks seam, and both halves of the
// two-phase structural extension.
struct SpawnerRig {
    loader::EventsDecl events;
    loader::PrefabSpawner spawner;

    explicit SpawnerRig(HostFixture& hf)
        : spawner(hf.fix.world,
                  hf.fix.hierarchy,
                  hf.fix.chart(),
                  hf.fix.bus(),
                  hf.fix.writer(),
                  hf.fix.registry,
                  events,
                  load_vocab(hf),
                  &hf.fix.loop()) {
        loader::SpawnOptions options;
        options.scripts = &unwrap(hf.host);
        options.defer_initial_entry = true;
        spawner.set_spawn_options(options);
        spawner.set_despawn_hooks(unwrap(hf.host));
        hf.fix.loop().set_structural_preparer(
            [this](std::uint64_t tick, std::uint64_t phase_record_id) {
                return spawner.prepare(tick, phase_record_id);
            });
        hf.fix.loop().set_structural_realizer(
            [this](std::uint64_t phase_record_id) { return spawner.realize(phase_record_id); });
    }

    static loader::ComponentVocab load_vocab(HostFixture& hf) {
        loader::ComponentVocabLoadResult loaded =
            loader::load_component_vocab(hf.scripts.file("schema_manifest.json"));
        REQUIRE_FALSE(loaded.error.has_value());
        return loaded.vocab;
    }
};

} // namespace

TEST_CASE("script.despawn_lifecycle: linger -> exit chains -> reap through the REAL host — "
          "order, silenced seats, and the script-visible reap tick (G2)") {
    HostFixture hf("despawn_lifecycle");

    // Two authored components: Pulse (base) binds poke.pulse and reports its
    // exits; Sentinel (state-owned by Alive) reports its exits. Both onExit
    // hooks receive "" at despawn — there is no destination state.
    const std::string pulse = hf.write_module(
        "pulse",
        "import {Component} from 'midday'\n"
        "export class Pulse extends Component {\n"
        "    onEvent(event: string, payload: unknown): void { this.emit('pulse.heard', {event}) }\n"
        "    onExit(to: string): void { this.emit('pulse.dying', {to}) }\n"
        "}\n");
    const std::string sentinel =
        hf.write_module("sentinel",
                        "import {Component} from 'midday'\n"
                        "export class Sentinel extends Component {\n"
                        "    onExit(to: string): void { this.emit('sentinel.exited', {to}) }\n"
                        "}\n");
    hf.load_manifest(
        manifest_entry(
            "Pulse", pulse, "", R"({"event": "poke.pulse", "payload_compat_hash": ""})") +
        "," + manifest_entry("Sentinel", sentinel, "", ""));

    // The prefab: base Pulse + a machine whose single Alive state owns
    // Sentinel. NO state scripts — the G1 rule; the chart exit chain plus
    // the base despawn_exit are exactly what a runtime prefab gets.
    REQUIRE_FALSE(base::write_file(hf.scripts.file("corpse.machine.yaml"),
                                   "format: 1\n"
                                   "machine: corpse\n"
                                   "regions:\n"
                                   "  life:\n"
                                   "    initial: Alive\n"
                                   "    states:\n"
                                   "      Alive:\n"
                                   "        components:\n"
                                   "          - Sentinel: {}\n",
                                   "t")
                      .has_value());
    const std::string prefab_path =
        std::filesystem::path(hf.scripts.file("corpse.entity.yaml")).generic_string();
    REQUIRE_FALSE(base::write_file(prefab_path,
                                   "format: 1\n"
                                   "entity: Corpse\n"
                                   "base:\n"
                                   "  - Pulse: {}\n"
                                   "machines:\n"
                                   "  - instance: {path: corpse.machine.yaml}\n",
                                   "t")
                      .has_value());

    SpawnerRig rig(hf);
    loader::PrefabSpawner& spawner = rig.spawner;

    loader::PrefabSpawnResult spawned = spawner.spawn_prefab(prefab_path, math::Vec3{});
    REQUIRE_MESSAGE(!spawned.error.has_value(),
                    (spawned.error ? spawned.error->message : std::string()));
    REQUIRE_FALSE(hf.fix.loop().tick().has_value()); // tick 1: realize + initial entry
    REQUIRE(hf.fix.world.alive(spawned.ref));
    REQUIRE_FALSE(unwrap(hf.host).first_error().has_value());
    CHECK(unwrap(hf.host).seat_count() == 2); // Pulse (base) + Sentinel (state)

    // The living seat hears its bound event.
    REQUIRE_FALSE(
        hf.fix.bus()
            .trigger(bus::EventKey::entity(spawned.ref), Name("poke.pulse"), Json::object(), 0)
            .error.has_value());

    // The linger: 0.1 * 60 is exactly 6.0 in IEEE doubles -> due tick
    // 1 + 6 = 7. The corpse stays fully alive through tick 6.
    REQUIRE_FALSE(spawner.despawn(spawned.ref, loader::DespawnOptions{0.1}).has_value());
    REQUIRE_FALSE(hf.fix.loop().run_to_tick(6).has_value());
    CHECK(hf.fix.world.alive(spawned.ref));
    REQUIRE_FALSE(hf.fix.loop().tick().has_value()); // tick 7: the due tick
    CHECK_FALSE(hf.fix.world.alive(spawned.ref));
    REQUIRE_FALSE(unwrap(hf.host).first_error().has_value());

    // Post-reap silence: the same trigger at the dead key reaches NO seat.
    REQUIRE_FALSE(
        hf.fix.bus()
            .trigger(bus::EventKey::entity(spawned.ref), Name("poke.pulse"), Json::object(), 0)
            .error.has_value());

    // G2, observable from SCRIPT: the stale-ref message names the reap tick.
    const std::string probe = hf.write_module(
        "stale_probe",
        "import {EntityRef} from 'midday/component'\n"
        "const ref = new EntityRef(" +
            std::to_string(spawned.ref.index) + ", " + std::to_string(spawned.ref.generation) +
            ")\n"
            "if (ref.alive) throw new Error('the corpse must be dead by now')\n"
            "let message = ''\n"
            "try { ref.root() } catch (error) { message = String((error as Error).message) }\n"
            "if (!message.includes('despawned at tick 7'))\n"
            "    throw new Error('stale message must carry the REAP tick, got: ' + message)\n");
    script::Toolchain::LoadOutcome outcome = unwrap(hf.toolchain).load_module(hf.runtime, probe);
    REQUIRE_MESSAGE(!outcome.error.has_value(),
                    (outcome.error ? outcome.error->message : std::string()));

    // The journal, in D4 order at tick 7: prefab.despawn -> the chart's
    // component_exit (Sentinel, through exit-3) -> component.despawn_exit
    // (Pulse, base) -> entity.despawned — each effect citing the record
    // before it, the whole chain rooted at the despawn record.
    std::vector<Record> records = hf.fix.finish();
    const std::vector<Record> despawns = of_kind(records, "prefab.despawn");
    REQUIRE(despawns.size() == 1);
    CHECK(despawns[0].tick == 7);
    CHECK(field(despawns[0].payload, "requested").as_int() == 1);
    CHECK(field(despawns[0].payload, "due").as_int() == 7);

    std::vector<Record> chart_exits;
    for (const Record& record : of_kind(records, "statechart.hook"))
        if (field(record.payload, "hook").as_string() == "component_exit")
            chart_exits.push_back(record);
    REQUIRE(chart_exits.size() == 1);
    CHECK(field(chart_exits[0].payload, "component").as_string() == "Sentinel");
    CHECK(chart_exits[0].cause_id == despawns[0].id);
    CHECK(chart_exits[0].payload.find("peer") == nullptr);

    const std::vector<Record> base_exits = of_kind(records, "component.despawn_exit");
    REQUIRE(base_exits.size() == 1);
    CHECK(field(base_exits[0].payload, "component").as_string() == "Pulse");
    CHECK(base_exits[0].cause_id == despawns[0].id);

    const std::vector<Record> despawned = triggers_of(records, "entity.despawned");
    REQUIRE(despawned.size() == 1);
    CHECK(despawned[0].tick == 7);
    CHECK(despawned[0].cause_id == despawns[0].id);

    const std::size_t at_despawn = index_of(records, despawns[0].id);
    const std::size_t at_chart_exit = index_of(records, chart_exits[0].id);
    const std::size_t at_base_exit = index_of(records, base_exits[0].id);
    const std::size_t at_event = index_of(records, despawned[0].id);
    CHECK(at_despawn < at_chart_exit);
    CHECK(at_chart_exit < at_base_exit);
    CHECK(at_base_exit < at_event);

    // The onExit hooks ran with "" as the peer and their emits chained off
    // the hook records that journaled them.
    const std::vector<Record> sentinel_exited = triggers_of(records, "sentinel.exited");
    REQUIRE(sentinel_exited.size() == 1);
    CHECK(field(field(sentinel_exited[0].payload, "payload"), "to").as_string().empty());
    CHECK(sentinel_exited[0].cause_id == chart_exits[0].id);
    const std::vector<Record> pulse_dying = triggers_of(records, "pulse.dying");
    REQUIRE(pulse_dying.size() == 1);
    CHECK(field(field(pulse_dying[0].payload, "payload"), "to").as_string().empty());
    CHECK(pulse_dying[0].cause_id == base_exits[0].id);

    // Delivery before the reap, silence after: ONE poke reached the seat.
    CHECK(triggers_of(records, "pulse.heard").size() == 1);
    CHECK(of_kind(records, "component.on_event").size() == 1);
}

TEST_CASE("script.despawn_lifecycle: overlapping same-tick lingers on NESTED entities run each "
          "base onExit exactly ONCE — the ancestor's subtree walk never re-exits the child") {
    HostFixture hf("despawn_nested");

    // One base component on BOTH entities: its onExit emit is the counter.
    const std::string mourner =
        hf.write_module("mourner",
                        "import {Component} from 'midday'\n"
                        "export class Mourner extends Component {\n"
                        "    onExit(to: string): void { this.emit('mourner.exited', {to}) }\n"
                        "}\n");
    hf.load_manifest(manifest_entry("Mourner", mourner, "", ""));

    REQUIRE_FALSE(base::write_file(hf.scripts.file("shell.machine.yaml"),
                                   "format: 1\n"
                                   "machine: shell\n"
                                   "regions:\n"
                                   "  main:\n"
                                   "    initial: Idle\n"
                                   "    states:\n"
                                   "      Idle: {}\n",
                                   "t")
                      .has_value());
    const std::string prefab_path =
        std::filesystem::path(hf.scripts.file("shell.entity.yaml")).generic_string();
    REQUIRE_FALSE(base::write_file(prefab_path,
                                   "format: 1\n"
                                   "entity: Shell\n"
                                   "base:\n"
                                   "  - Mourner: {}\n"
                                   "machines:\n"
                                   "  - instance: {path: shell.machine.yaml}\n",
                                   "t")
                      .has_value());

    SpawnerRig rig(hf);
    loader::PrefabSpawner& spawner = rig.spawner;

    loader::PrefabSpawnResult parent = spawner.spawn_prefab(prefab_path, math::Vec3{});
    loader::PrefabSpawnResult child = spawner.spawn_prefab(prefab_path, math::Vec3{});
    REQUIRE_MESSAGE(!parent.error.has_value(),
                    (parent.error ? parent.error->message : std::string()));
    REQUIRE_FALSE(child.error.has_value());
    REQUIRE_FALSE(hf.fix.loop().tick().has_value()); // tick 1: both realize as roots
    REQUIRE(hf.fix.world.alive(parent.ref));
    REQUIRE(hf.fix.world.alive(child.ref));

    // Nest them: the child re-parents under the parent at tick 2's flush.
    REQUIRE_FALSE(hf.fix.hierarchy.queue_attach(child.ref, parent.ref).has_value());
    REQUIRE_FALSE(hf.fix.loop().tick().has_value()); // tick 2: attached
    CHECK(hf.fix.hierarchy.parent_of(child.ref) == parent.ref);

    // The overlap (bus tick 2): CHILD requested first, then its ancestor,
    // both 0.05 * 60 = 3.0 -> due tick 2 + 3 = 5. At tick 5's prepare the
    // child's own entry exits it; the parent's subtree walk then re-reaches
    // the still-unflushed child — which must NOT exit again.
    REQUIRE_FALSE(spawner.despawn(child.ref, loader::DespawnOptions{0.05}).has_value());
    REQUIRE_FALSE(spawner.despawn(parent.ref, loader::DespawnOptions{0.05}).has_value());
    REQUIRE_FALSE(hf.fix.loop().run_to_tick(5).has_value());
    CHECK_FALSE(hf.fix.world.alive(parent.ref));
    CHECK_FALSE(hf.fix.world.alive(child.ref));
    REQUIRE_FALSE(unwrap(hf.host).first_error().has_value());

    std::vector<Record> records = hf.fix.finish();
    const std::vector<Record> despawns = of_kind(records, "prefab.despawn");
    REQUIRE(despawns.size() == 2); // request order: the child's, then the parent's
    CHECK(despawns[0].tick == 5);
    CHECK(despawns[1].tick == 5);

    // EXACTLY one base onExit per component instance — the child's under
    // ITS OWN despawn record (the first walk), the parent's under its own.
    auto entity_form = [](const ecs::EntityRef ref) {
        return "entity:" + std::to_string(ref.index) + "#" + std::to_string(ref.generation);
    };
    const std::vector<Record> base_exits = of_kind(records, "component.despawn_exit");
    REQUIRE(base_exits.size() == 2);
    CHECK(field(base_exits[0].payload, "entity").as_string() == entity_form(child.ref));
    CHECK(base_exits[0].cause_id == despawns[0].id);
    CHECK(field(base_exits[1].payload, "entity").as_string() == entity_form(parent.ref));
    CHECK(base_exits[1].cause_id == despawns[1].id);
    CHECK(triggers_of(records, "mourner.exited").size() == 2); // one emit per instance
}
