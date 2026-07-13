// script.component_instance (loader path) — the whole authored pipeline
// with the REAL ComponentInstanceHost: vocab from the same manifest the
// host loads, load_scene, spawn_scene with the dispatcher + deferred entry,
// start_initial_entries, then a typed contact.began driving the D6 shape —
// base Transform MIRROR read via entity.get(Transform), payload hydration,
// golden.kill, and the state component's exit riding the chain.

#include "core/loader/loader.h"
#include "ts/runtime/component_instance_test_support.h"

#include <string>
#include <vector>

using namespace midday;
using midday::base::Json;
using midday::base::Name;
using midday::ecs::EntityRef;
using midday::journal::Record;
using midday::script::test::HostFixture;
using midday::script::test::manifest_entry;
using midday::script::test::triggers_of;
using midday::statechart::test::field;
using midday::testkit::unwrap;

TEST_CASE("script.component_instance: the whole loader path — prefab base Transform mirror + "
          "typed contact -> golden.kill -> exit chain (the D6 shape)") {
    HostFixture hf("loader_path");

    const std::string module =
        hf.write_module("contact_relay",
                        "import {Component, Transform} from 'midday'\n"
                        "export class ContactRelay extends Component {\n"
                        "    expectedX = 0\n"
                        "    onEvent(event: string, payload: unknown): void {\n"
                        "        const p = payload as {position: {x: number}}\n"
                        "        const tx = this.entity.get(Transform).position.x\n"
                        "        this.emit('relay.verify', {event, x: p.position.x, tx,\n"
                        "                                   expected: this.expectedX})\n"
                        "        if (event === 'contact.began' && tx === this.expectedX)\n"
                        "            this.emit('golden.kill', {})\n"
                        "    }\n"
                        "}\n");
    const std::string sentinel =
        hf.write_module("sentinel",
                        "import {Component} from 'midday'\n"
                        "export class Sentinel extends Component {\n"
                        "    onExit(to: string): void { this.emit('sentinel.exited', {to}) }\n"
                        "}\n");
    hf.load_manifest(
        manifest_entry("ContactRelay",
                       module,
                       R"({"name": "expectedX", "type": "float"})",
                       R"({"event": "contact.began", "payload_compat_hash": "08d68516245c6356"})") +
        "," + manifest_entry("Sentinel", sentinel, "", ""));

    // The authored corpus: golden.kill declared, a prefab with base
    // Transform + ContactRelay, a machine whose Alive state owns Sentinel.
    REQUIRE_FALSE(base::write_file(hf.scripts.file("probe.events.yaml"),
                                   "format: 1\n"
                                   "events:\n"
                                   "  golden.kill: {payload: {}}\n",
                                   "t")
                      .has_value());
    REQUIRE_FALSE(base::write_file(hf.scripts.file("probe.machine.yaml"),
                                   "format: 1\n"
                                   "machine: probe\n"
                                   "regions:\n"
                                   "  life:\n"
                                   "    initial: Alive\n"
                                   "    states:\n"
                                   "      Alive:\n"
                                   "        components:\n"
                                   "          - Sentinel: {}\n"
                                   "        on:\n"
                                   "          - {event: golden.kill, goto: Dead}\n"
                                   "      Dead: {}\n",
                                   "t")
                      .has_value());
    REQUIRE_FALSE(base::write_file(hf.scripts.file("probe.entity.yaml"),
                                   "format: 1\n"
                                   "entity: LifecycleProbe\n"
                                   "base:\n"
                                   "  - Transform: {at: [7, 0, 3]}\n"
                                   "  - ContactRelay: {expectedX: 7}\n"
                                   "machines:\n"
                                   "  - instance: {path: probe.machine.yaml}\n",
                                   "t")
                      .has_value());
    REQUIRE_FALSE(base::write_file(hf.scripts.file("arena.scene.yaml"),
                                   "format: 1\n"
                                   "scene: arena\n"
                                   "events: [probe.events.yaml]\n"
                                   "entities:\n"
                                   "  - entity: Probe\n"
                                   "    prefab: {path: probe.entity.yaml}\n",
                                   "t")
                      .has_value());

    // The REAL vocabulary loader reads the same manifest the host loaded;
    // the shared helper drives the whole production boot shape.
    loader::SpawnResult spawned =
        script::test::spawn_component_scene(hf, hf.scripts.file("arena.scene.yaml"));

    // The typed contact at the probe's channel: distinct other, x = 7.
    const EntityRef probe = spawned.machines.at(0).host;
    const EntityRef other = hf.fix.world.spawn();
    Json contact = Json::object();
    contact.set("self", static_cast<std::int64_t>(probe.to_bits()));
    contact.set("other", static_cast<std::int64_t>(other.to_bits()));
    Json position = Json::array();
    position.push(7.0);
    position.push(0.0);
    position.push(3.0);
    contact.set("position", std::move(position));
    Json normal = Json::array();
    normal.push(0.0);
    normal.push(1.0);
    normal.push(0.0);
    contact.set("normal", std::move(normal));
    contact.set("impulse", 0.5);
    REQUIRE_FALSE(hf.fix.bus()
                      .trigger(bus::EventKey::entity(probe), Name("contact.began"), contact, 0)
                      .error.has_value());

    // The relay read the MIRRORED base Transform (x == 7), verified the
    // payload, and killed the machine; Sentinel's exit rode the chain.
    CHECK(hf.fix.chart().in_state(spawned.machines.at(0).id, Name("life"), Name("Dead")));
    REQUIRE_FALSE(unwrap(hf.host).first_error().has_value());
    std::vector<Record> records = hf.fix.finish();
    const std::vector<Record> verified = triggers_of(records, "relay.verify");
    REQUIRE(verified.size() == 1);
    const Json& verdict = field(verified[0].payload, "payload");
    CHECK(field(verdict, "event").as_string() == "contact.began");
    CHECK(field(verdict, "x").as_double() == doctest::Approx(7.0));
    CHECK(field(verdict, "tx").as_double() == doctest::Approx(7.0));
    CHECK(triggers_of(records, "golden.kill").size() == 1);
    const std::vector<Record> exited = triggers_of(records, "sentinel.exited");
    REQUIRE(exited.size() == 1);
    CHECK(field(field(exited[0].payload, "payload"), "to").as_string() == "Dead");
}

TEST_CASE("script.component_instance: EVERY native-Transform seed mirrors for script components "
          "(council fix G6) — scene INLINE `Transform:` and prefab `at:` both read back through "
          "entity.get(Transform)") {
    HostFixture hf("seed_mirror");

    // The reader reports exactly what the mirror holds.
    const std::string module =
        hf.write_module("reader",
                        "import {Component, Transform} from 'midday'\n"
                        "export class Reader extends Component {\n"
                        "    onEvent(event: string, payload: unknown): void {\n"
                        "        void event; void payload;\n"
                        "        const t = this.entity.get(Transform)\n"
                        "        this.emit('mirror.report',\n"
                        "                  {x: t.position.x, y: t.position.y, z: t.position.z})\n"
                        "    }\n"
                        "}\n");
    hf.load_manifest(manifest_entry(
        "Reader", module, "", R"({"event": "mirror.ping", "payload_compat_hash": ""})"));

    // A minimal machine on both entities: spawned.machines carries the host
    // refs the triggers need (the loader-path test's own trick).
    REQUIRE_FALSE(base::write_file(hf.scripts.file("idle.machine.yaml"),
                                   "format: 1\n"
                                   "machine: idle\n"
                                   "regions:\n"
                                   "  life:\n"
                                   "    initial: Alive\n"
                                   "    states:\n"
                                   "      Alive: {}\n",
                                   "t")
                      .has_value());
    // Beta: NO generic Transform in base — placement comes from the scene
    // instance's `at:` alone (the exact pre-fix script.missing_component
    // hole; alpha covers the inline-authoring flavor of the same hole).
    REQUIRE_FALSE(base::write_file(hf.scripts.file("beta.entity.yaml"),
                                   "format: 1\n"
                                   "entity: Beta\n"
                                   "base:\n"
                                   "  - Reader: {}\n"
                                   "machines:\n"
                                   "  - instance: {path: idle.machine.yaml}\n",
                                   "t")
                      .has_value());
    REQUIRE_FALSE(base::write_file(hf.scripts.file("mirror.scene.yaml"),
                                   "format: 1\n"
                                   "scene: mirror\n"
                                   "entities:\n"
                                   "  - entity: Alpha\n"
                                   "    components:\n"
                                   "      - Transform: {at: [7, 0, 3]}\n"
                                   "      - Reader: {}\n"
                                   "    machines:\n"
                                   "      - instance: {path: idle.machine.yaml}\n"
                                   "  - entity: Beta\n"
                                   "    prefab: {path: beta.entity.yaml}\n"
                                   "    at: [4, 5, 6]\n",
                                   "t")
                      .has_value());

    loader::SpawnResult spawned =
        script::test::spawn_component_scene(hf, hf.scripts.file("mirror.scene.yaml"));
    REQUIRE(spawned.machines.size() == 2);

    // Pre-fix: BOTH gets threw script.missing_component (only the generic
    // `Transform: {at:}` entry ever mirrored). Now each seed reads back.
    for (const loader::MachineSeat& seat : spawned.machines)
        REQUIRE_FALSE(
            hf.fix.bus()
                .trigger(bus::EventKey::entity(seat.host), Name("mirror.ping"), Json::object(), 0)
                .error.has_value());
    REQUIRE_FALSE(unwrap(hf.host).first_error().has_value());

    std::vector<Record> records = hf.fix.finish();
    const std::vector<Record> reports = triggers_of(records, "mirror.report");
    REQUIRE(reports.size() == 2); // trigger order: Alpha, then Beta
    const Json& alpha = field(reports[0].payload, "payload");
    CHECK(field(alpha, "x").as_double() == doctest::Approx(7.0));
    CHECK(field(alpha, "y").as_double() == doctest::Approx(0.0));
    CHECK(field(alpha, "z").as_double() == doctest::Approx(3.0));
    const Json& beta = field(reports[1].payload, "payload");
    CHECK(field(beta, "x").as_double() == doctest::Approx(4.0));
    CHECK(field(beta, "y").as_double() == doctest::Approx(5.0));
    CHECK(field(beta, "z").as_double() == doctest::Approx(6.0));
}
