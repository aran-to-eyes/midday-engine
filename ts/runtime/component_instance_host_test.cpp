// script.component_instance — the TS component instance host (M2 0B, #12b):
// typed payload hydration (EntityRef fields become EntityRef instances with
// the RIGHT identities, vec3 fields their {x,y,z} shape), manifest binding
// filter, dispatch order = subscription order = materialization order (D1),
// state-scoped lifecycle through the chart's REAL enter-2/exit-3 slots
// (subscribe at enter in attach order, onExit then unsubscribe at exit in
// reverse), typed field hydration with structured refusals, cause chains
// (hook/dispatch record -> the emits it caused), and reap. Every seat runs
// a REAL TS module through the toolchain on the SIM runtime — nothing here
// fakes the boundary.

#include "core/base/hex.h"
#include "ts/runtime/component_instance_test_support.h"

#include <string>
#include <vector>

using namespace midday;
using midday::base::Error;
using midday::base::Json;
using midday::base::Name;
using midday::ecs::EntityRef;
using midday::journal::Record;
using midday::script::test::HostFixture;
using midday::script::test::manifest_entry;
using midday::script::test::triggers_of;
using midday::statechart::test::field;
using midday::statechart::test::of_kind;
using midday::testkit::unwrap;

TEST_CASE("script.component_instance: typed hydration (distinct EntityRefs, vec3, fields), "
          "binding filter, dispatch order = attach order") {
    HostFixture hf("hydrate");
    const EntityRef probe = hf.fix.world.spawn();
    const EntityRef other = hf.fix.world.spawn();
    REQUIRE_FALSE(probe.is_null());
    REQUIRE_FALSE(other.is_null());

    const std::string module = hf.write_module(
        "relay",
        "import {Component} from 'midday'\n"
        "export class RelayA extends Component {\n"
        "    expectedX = 0\n"
        "    onEvent(event: string, payload: unknown): void {\n"
        "        const p = payload as {self: {index: number}, other: {index: number},\n"
        "                              position: {x: number, y: number, z: number}}\n"
        "        this.emit('relay.echo', {who: 'A', event, x: p.position.x,\n"
        "                                 selfIndex: p.self.index, otherIndex: p.other.index,\n"
        "                                 expected: this.expectedX})\n"
        "    }\n"
        "}\n"
        "export class RelayB extends Component {\n"
        "    onEvent(event: string, payload: unknown): void {\n"
        "        const p = payload as {note?: string}\n"
        "        this.emit('relay.echo', {who: 'B', event, note: p.note ?? ''})\n"
        "    }\n"
        "}\n");
    hf.load_manifest(
        manifest_entry("RelayA",
                       module,
                       R"({"name": "expectedX", "type": "float"})",
                       R"({"event": "contact.began", "payload_compat_hash": "08d68516245c6356"})") +
        "," +
        manifest_entry("RelayB",
                       module,
                       "",
                       R"({"event": "contact.began", "payload_compat_hash": "08d68516245c6356"},)"
                       R"({"event": "pulse.ping", "payload_compat_hash": ""})"));

    // Materialize A then B (authored order): that IS the dispatch order.
    loader::GenericComponentEntry relay_a;
    relay_a.type = Name("RelayA");
    relay_a.fields.set("expectedX", 7.0);
    loader::GenericComponentEntry relay_b;
    relay_b.type = Name("RelayB");
    REQUIRE_FALSE(unwrap(hf.host).materialize_base(probe, relay_a, 0).has_value());
    REQUIRE_FALSE(unwrap(hf.host).materialize_base(probe, relay_b, 0).has_value());
    CHECK(unwrap(hf.host).seat_count() == 2);

    // A REGISTERED event with entity_ref + vec3 fields (runtime wire shape;
    // DISTINCT self/other — a swapped hydration is observable).
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

    // An UNREGISTERED custom event: only RelayB binds it; payload verbatim.
    Json ping = Json::object();
    ping.set("note", "hi");
    REQUIRE_FALSE(hf.fix.bus()
                      .trigger(bus::EventKey::entity(probe), Name("pulse.ping"), ping, 0)
                      .error.has_value());

    // A registered event NEITHER binds: filtered out, no dispatch records.
    Json ended = Json::object();
    ended.set("self", static_cast<std::int64_t>(probe.to_bits()));
    ended.set("other", static_cast<std::int64_t>(other.to_bits()));
    REQUIRE_FALSE(hf.fix.bus()
                      .trigger(bus::EventKey::entity(probe), Name("contact.ended"), ended, 0)
                      .error.has_value());

    REQUIRE_FALSE(unwrap(hf.host).first_error().has_value());
    std::vector<Record> records = hf.fix.finish();

    // Dispatch records: A then B for contact.began, B alone for pulse.ping.
    const std::vector<Record> dispatches = of_kind(records, "component.on_event");
    REQUIRE(dispatches.size() == 3);
    CHECK(field(dispatches[0].payload, "component").as_string() == "RelayA");
    CHECK(field(dispatches[0].payload, "event").as_string() == "contact.began");
    CHECK(field(dispatches[1].payload, "component").as_string() == "RelayB");
    CHECK(field(dispatches[2].payload, "component").as_string() == "RelayB");
    CHECK(field(dispatches[2].payload, "event").as_string() == "pulse.ping");

    // The echoes prove hydration: EntityRef identities distinct and right,
    // vec3 in {x,y,z} shape, the authored float field applied — and each
    // echo's CAUSE is its dispatch record (the cause chain).
    const std::vector<Record> echoes = triggers_of(records, "relay.echo");
    REQUIRE(echoes.size() == 3);
    const Json& first = field(echoes[0].payload, "payload");
    CHECK(field(first, "who").as_string() == "A");
    CHECK(field(first, "x").as_double() == doctest::Approx(7.0));
    CHECK(field(first, "selfIndex").as_int() == static_cast<std::int64_t>(probe.index));
    CHECK(field(first, "otherIndex").as_int() == static_cast<std::int64_t>(other.index));
    CHECK(field(first, "expected").as_double() == doctest::Approx(7.0));
    CHECK(echoes[0].cause_id == dispatches[0].id);
    const Json& second = field(echoes[1].payload, "payload");
    CHECK(field(second, "who").as_string() == "B");
    CHECK(echoes[1].cause_id == dispatches[1].id);
    const Json& third = field(echoes[2].payload, "payload");
    CHECK(field(third, "note").as_string() == "hi"); // verbatim: no schema, no hydration
    CHECK(echoes[2].cause_id == dispatches[2].id);
}

TEST_CASE("script.component_instance: state seats ride the REAL enter-2/exit-3 slots — "
          "subscribe at enter, reverse onExit at exit, silent after") {
    HostFixture hf("lifecycle");
    statechart::Statechart& chart = hf.fix.chart();

    const std::string module = hf.write_module(
        "pulse",
        "import {Component} from 'midday'\n"
        "class PulseBase extends Component {\n"
        "    who = ''\n"
        "    onEnter(from: string): void { this.emit('pulse.entered', {who: this.who, from}) }\n"
        "    onExit(to: string): void { this.emit('pulse.exited', {who: this.who, to}) }\n"
        "    onEvent(event: string, payload: unknown): void {\n"
        "        void event; void payload;\n"
        "        this.emit('pulse.heard', {who: this.who})\n"
        "    }\n"
        "}\n"
        "export class PulseA extends PulseBase { who = 'A' }\n"
        "export class PulseB extends PulseBase { who = 'B' }\n");
    const std::string binding = R"({"event": "pulse.ping", "payload_compat_hash": ""})";
    hf.load_manifest(manifest_entry("PulseA", module, "", binding) + "," +
                     manifest_entry("PulseB", module, "", binding));

    // Alive machine with a kill exit; components seat BEFORE initial entry
    // (the split) through the real materializer API.
    statechart::MachineDesc desc = statechart::test::machine(
        "probe",
        {statechart::test::region(
            "life",
            "Alive",
            {statechart::test::state("Alive", {statechart::test::pair("golden.kill", "Dead")}),
             statechart::test::state("Dead")})});
    statechart::InstantiateOptions defer;
    defer.defer_initial_entry = true;
    statechart::InstantiateResult made = chart.instantiate(desc, hf.fix.host, 0, defer);
    REQUIRE_FALSE(made.error.has_value());

    statechart::StateComponentDesc pulse_a;
    pulse_a.type = Name("PulseA");
    pulse_a.fields = Json::object();
    statechart::StateComponentDesc pulse_b;
    pulse_b.type = Name("PulseB");
    pulse_b.fields = Json::object();
    REQUIRE_FALSE(unwrap(hf.host)
                      .materialize_state(
                          chart, made.machine, Name("life"), Name("Alive"), hf.fix.host, pulse_a, 0)
                      .has_value());
    REQUIRE_FALSE(unwrap(hf.host)
                      .materialize_state(
                          chart, made.machine, Name("life"), Name("Alive"), hf.fix.host, pulse_b, 0)
                      .has_value());
    REQUIRE_FALSE(chart.start_initial_entry(made.machine).has_value());

    // Active state components hear their bound event, attach order.
    REQUIRE_FALSE(hf.fix.trigger("pulse.ping").error.has_value());
    // The kill: exit-3 runs onExit REVERSE (B then A) and unsubscribes.
    REQUIRE_FALSE(hf.fix.trigger("golden.kill").error.has_value());
    CHECK(chart.in_state(made.machine, Name("life"), Name("Dead")));
    // Dormant seats: the same event now reaches NOTHING.
    REQUIRE_FALSE(hf.fix.trigger("pulse.ping").error.has_value());

    REQUIRE_FALSE(unwrap(hf.host).first_error().has_value());
    std::vector<Record> records = hf.fix.finish();

    const std::vector<Record> entered = triggers_of(records, "pulse.entered");
    REQUIRE(entered.size() == 2); // attach order at enter-2
    CHECK(field(field(entered[0].payload, "payload"), "who").as_string() == "A");
    CHECK(field(field(entered[1].payload, "payload"), "who").as_string() == "B");

    const std::vector<Record> heard = triggers_of(records, "pulse.heard");
    REQUIRE(heard.size() == 2); // once each, subscription order, never after exit
    CHECK(field(field(heard[0].payload, "payload"), "who").as_string() == "A");
    CHECK(field(field(heard[1].payload, "payload"), "who").as_string() == "B");

    const std::vector<Record> exited = triggers_of(records, "pulse.exited");
    REQUIRE(exited.size() == 2); // REVERSE attach order at exit-3
    CHECK(field(field(exited[0].payload, "payload"), "who").as_string() == "B");
    CHECK(field(field(exited[1].payload, "payload"), "who").as_string() == "A");
    CHECK(field(field(exited[0].payload, "payload"), "to").as_string() == "Dead");

    // Cause chains: each enter emit cites ITS component_enter hook record.
    const std::vector<Record> hooks = of_kind(records, "statechart.hook");
    std::vector<std::uint64_t> component_enter_ids;
    for (const Record& record : hooks)
        if (field(record.payload, "hook").as_string() == "component_enter")
            component_enter_ids.push_back(record.id);
    REQUIRE(component_enter_ids.size() == 2);
    CHECK(entered[0].cause_id == component_enter_ids[0]);
    CHECK(entered[1].cause_id == component_enter_ids[1]);
}

TEST_CASE("script.component_instance: field refusals are structured; reap silences and releases") {
    HostFixture hf("refuse");
    const EntityRef probe = hf.fix.world.spawn();
    REQUIRE_FALSE(probe.is_null());

    const std::string module =
        hf.write_module("counter",
                        "import {Component} from 'midday'\n"
                        "export class Counter extends Component {\n"
                        "    rate = 1\n"
                        "    onExit(to: string): void { this.emit('counter.exit', {to}) }\n"
                        "    onEvent(event: string, payload: unknown): void {\n"
                        "        void event; void payload;\n"
                        "        this.emit('counter.tick', {})\n"
                        "    }\n"
                        "}\n");
    hf.load_manifest(manifest_entry("Counter",
                                    module,
                                    R"({"name": "rate", "type": "int"})",
                                    R"({"event": "pulse.ping", "payload_compat_hash": ""})"));

    // Unknown component name: fail-closed, structured.
    loader::GenericComponentEntry ghost;
    ghost.type = Name("Ghost");
    CHECK(unwrap(unwrap(hf.host).materialize_base(probe, ghost, 0)).code ==
          "component.not_in_manifest");

    // Unknown / mistyped authored fields refuse BEFORE any attach.
    loader::GenericComponentEntry bad_field;
    bad_field.type = Name("Counter");
    bad_field.fields.set("rte", 2);
    CHECK(unwrap(unwrap(hf.host).materialize_base(probe, bad_field, 0)).code ==
          "component.unknown_field");
    loader::GenericComponentEntry bad_type;
    bad_type.type = Name("Counter");
    bad_type.fields.set("rate", "fast");
    CHECK(unwrap(unwrap(hf.host).materialize_base(probe, bad_type, 0)).code ==
          "component.field_type");

    // A good seat hears; the despawn path runs its base onExit exactly
    // once (despawn_exit), then reap silences and releases it.
    loader::GenericComponentEntry good;
    good.type = Name("Counter");
    good.fields.set("rate", 2);
    REQUIRE_FALSE(unwrap(hf.host).materialize_base(probe, good, 0).has_value());
    REQUIRE_FALSE(hf.fix.bus()
                      .trigger(bus::EventKey::entity(probe), Name("pulse.ping"), Json::object(), 0)
                      .error.has_value());
    unwrap(hf.host).despawn_exit(probe, 0);
    unwrap(hf.host).despawn_exit(probe, 0); // overlapping reap walk (nested lingers): no repeat
    unwrap(hf.host).reap_entity(probe);
    unwrap(hf.host).despawn_exit(probe, 0); // released: no second onExit
    REQUIRE_FALSE(hf.fix.bus()
                      .trigger(bus::EventKey::entity(probe), Name("pulse.ping"), Json::object(), 0)
                      .error.has_value());

    REQUIRE_FALSE(unwrap(hf.host).first_error().has_value());
    std::vector<Record> records = hf.fix.finish();
    CHECK(triggers_of(records, "counter.tick").size() == 1); // heard once, silent after reap
    CHECK(of_kind(records, "component.on_event").size() == 1);
    const std::vector<Record> exits = triggers_of(records, "counter.exit");
    REQUIRE(exits.size() == 1); // despawn_exit exactly once, never after reap
    const std::vector<Record> despawn_exits = of_kind(records, "component.despawn_exit");
    REQUIRE(despawn_exits.size() == 1);
    CHECK(exits[0].cause_id == despawn_exits[0].id); // the emit cites its hook record
}

TEST_CASE("script.component_instance: host destruction unsubscribes every still-live seat "
          "listener — the bus survives the host with no dangling entries (0A teardown fence)") {
    HostFixture hf("teardown");
    const EntityRef probe = hf.fix.world.spawn();
    const EntityRef other = hf.fix.world.spawn();
    REQUIRE_FALSE(probe.is_null());
    REQUIRE_FALSE(other.is_null());

    const std::string module = hf.write_module(
        "lingerer",
        "import {Component} from 'midday'\n"
        "export class Lingerer extends Component {\n"
        "    onEvent(event: string, payload: unknown): void { void event; void payload; }\n"
        "}\n");
    hf.load_manifest(manifest_entry(
        "Lingerer", module, "", R"({"event": "pulse.ping", "payload_compat_hash": ""})"));

    loader::GenericComponentEntry entry;
    entry.type = Name("Lingerer");
    REQUIRE_FALSE(unwrap(hf.host).materialize_base(probe, entry, 0).has_value());
    REQUIRE_FALSE(unwrap(hf.host).materialize_base(other, entry, 0).has_value());
    CHECK(hf.fix.bus().subscriber_count(bus::EventKey::entity(probe)) == 1);
    CHECK(hf.fix.bus().subscriber_count(bus::EventKey::entity(other)) == 1);

    // One seat reaped the normal way (already unsubscribed), one still live
    // at "shutdown": the destructor must clear the live one and no-op the
    // released one.
    unwrap(hf.host).reap_entity(other);
    CHECK(hf.fix.bus().subscriber_count(bus::EventKey::entity(other)) == 0);
    REQUIRE_FALSE(unwrap(hf.host).first_error().has_value());

    // The RunSim teardown order: the host dies first, the bus survives it —
    // and holds no entity-listener entry for any of the host's seats.
    hf.host.reset();
    CHECK(hf.fix.bus().subscriber_count(bus::EventKey::entity(probe)) == 0);
    CHECK(hf.fix.bus().subscriber_count(bus::EventKey::entity(other)) == 0);

    // A dispatch at the still-alive entity's key reaches NOTHING: no
    // dangling SeatListener remains for the generation gate to admit.
    REQUIRE_FALSE(hf.fix.bus()
                      .trigger(bus::EventKey::entity(probe), Name("pulse.ping"), Json::object(), 0)
                      .error.has_value());
    std::vector<Record> records = hf.fix.finish();
    CHECK(of_kind(records, "component.on_event").empty());
}

TEST_CASE("script.component_instance: manifest identity is FAIL-CLOSED (council fix G7) — wrong "
          "format_version and stale payload_compat_hash refuse at load, never bind silently") {
    HostFixture hf("manifest_gate");
    auto write_manifest = [&hf](const std::string& stem, const std::string& body) {
        const std::string path = hf.scripts.file(stem + ".components.json");
        REQUIRE_FALSE(base::write_file(path, body, "test.io").has_value());
        return path;
    };
    // The engine's own pin for a builtin event — what a fresh extraction
    // carries (the D6 corpus commits exactly this pair for contact.began).
    const reflect::Registered<reflect::EventDesc>* contact =
        hf.fix.registry.find_event(Name("contact.began"));
    REQUIRE(contact != nullptr);
    const std::string good_hash = base::hex64(contact->desc.compat_hash);
    CHECK(good_hash == "08d68516245c6356"); // the committed corpus value

    SUBCASE("format_version 999 refuses, naming found vs expected") {
        const std::string path =
            write_manifest("v999", R"({"format_version": 999, "components": []})");
        const Error error = unwrap(unwrap(hf.host).load_manifest(path));
        CHECK(error.code == "component.manifest_version");
        CHECK(field(error.details, "found").as_int() == 999);
        CHECK(field(error.details, "expected").as_int() == 2);
    }

    SUBCASE("a missing format_version refuses the same way (found: null)") {
        const std::string path = write_manifest("unversioned", R"({"components": []})");
        const Error error = unwrap(unwrap(hf.host).load_manifest(path));
        CHECK(error.code == "component.manifest_version");
        CHECK(field(error.details, "found").is_null());
    }

    SUBCASE("a tampered payload_compat_hash on a REGISTERED event refuses, naming the event "
            "and both hashes") {
        const std::string path = write_manifest(
            "tampered",
            R"({"format_version": 2, "components": [{"name": "Relay", "file": "relay.ts",)"
            R"( "fields": [], "event_bindings": [{"event": "contact.began",)"
            R"( "payload_compat_hash": "deadbeefdeadbeef"}]}]})");
        const Error error = unwrap(unwrap(hf.host).load_manifest(path));
        CHECK(error.code == "component.payload_hash");
        CHECK(field(error.details, "event").as_string() == "contact.began");
        CHECK(field(error.details, "found").as_string() == "deadbeefdeadbeef");
        CHECK(field(error.details, "expected").as_string() == good_hash);
    }

    SUBCASE("the correct pair loads; unregistered events stay the D-BUILD-046 pass-through "
            "(no engine schema, no hash check)") {
        const std::string path = write_manifest(
            "green",
            R"({"format_version": 2, "components": [{"name": "Relay", "file": "relay.ts",)"
            R"( "fields": [], "event_bindings": [{"event": "contact.began",)"
            R"( "payload_compat_hash": ")" +
                good_hash + R"("}, {"event": "pulse.ping", "payload_compat_hash": ""}]}]})");
        CHECK_FALSE(unwrap(hf.host).load_manifest(path).has_value());
    }
    (void)hf.fix.finish();
}

TEST_CASE("script.component_instance: a map payload key '__proto__' hydrates as an OWN property "
          "(council fix G8) — never a prototype graft, never a dropped field") {
    HostFixture hf("proto_key");
    const EntityRef probe = hf.fix.world.spawn();
    REQUIRE_FALSE(probe.is_null());

    // The decoded map reaches the class as a typed FIELD (the same decoder
    // arm hydrates map-typed event payload fields). The probe reports what
    // actually arrived: the own keys, the '__proto__' entry's value, and
    // whether anything leaked onto the prototype chain.
    const std::string module = hf.write_module(
        "bag_probe",
        "import {Component} from 'midday'\n"
        "export class BagProbe extends Component {\n"
        "    bag: Record<string, number> = {}\n"
        "    onEvent(event: string, payload: unknown): void {\n"
        "        void event; void payload;\n"
        "        const own = Object.prototype.hasOwnProperty.call(this.bag, '__proto__')\n"
        "        const keys = Object.keys(this.bag).sort().join(',')\n"
        "        const value = (this.bag as Record<string, unknown>)['__proto__']\n"
        "        this.emit('bag.report', {own, keys,\n"
        "                                 value: typeof value === 'number' ? value : -1,\n"
        "                                 grafted: Object.getPrototypeOf(this.bag) !== null})\n"
        "    }\n"
        "}\n");
    hf.load_manifest(manifest_entry("BagProbe",
                                    module,
                                    R"({"name": "bag", "type": "map<float>"})",
                                    R"({"event": "bag.poke", "payload_compat_hash": ""})"));

    loader::GenericComponentEntry entry;
    entry.type = Name("BagProbe");
    Json bag = Json::object();
    bag.set("__proto__", 9.0);
    bag.set("regular", 2.0);
    entry.fields.set("bag", std::move(bag));
    REQUIRE_FALSE(unwrap(hf.host).materialize_base(probe, entry, 0).has_value());
    REQUIRE_FALSE(hf.fix.bus()
                      .trigger(bus::EventKey::entity(probe), Name("bag.poke"), Json::object(), 0)
                      .error.has_value());

    REQUIRE_FALSE(unwrap(hf.host).first_error().has_value());
    std::vector<Record> records = hf.fix.finish();
    const std::vector<Record> reports = triggers_of(records, "bag.report");
    REQUIRE(reports.size() == 1);
    const Json& verdict = field(reports[0].payload, "payload");
    CHECK(field(verdict, "own").as_bool()); // the key survived as data
    CHECK(field(verdict, "keys").as_string() == "__proto__,regular");
    CHECK(field(verdict, "value").as_double() == doctest::Approx(9.0));
    CHECK_FALSE(field(verdict, "grafted").as_bool()); // no author-controlled prototype
}
