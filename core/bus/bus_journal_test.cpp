// bus.journal / bus.determinism — the bus's journal contract: byte/field-
// pinned event.trigger records, cause-id chaining walked back through the
// journal (trigger -> effect -> effect), the cascade-depth refusal record
// closing a 32-deep chain, refusal records for invalid payloads, the
// no-record-no-dispatch rule, and dual-run record identity (two independent
// buses driven by the same script, diffed — never a self-diff).

#include "core/base/hex.h"
#include "core/base/json.h"
#include "core/base/name.h"
#include "core/bus/bus.h"
#include "core/bus/test_support.h"
#include "core/journal/record.h"
#include "core/reflect/payload_codec.h"
#include "testkit/doctest.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using midday::base::Json;
using midday::base::Name;
using midday::bus::Bus;
using midday::bus::EventKey;
using midday::bus::EventView;
using midday::bus::TriggerResult;
using midday::bus::test::BusFixture;
using midday::bus::test::code_of;
using midday::bus::test::field;
using midday::bus::test::RecordingListener;
using midday::journal::Record;
using midday::testkit::unwrap;

TEST_CASE("bus.journal: the event.trigger record is byte-pinned, written BEFORE dispatch") {
    BusFixture fix;
    Bus& bus = fix.bus();
    const EventKey key = EventKey::named(Name("alpha"));

    std::vector<std::string> log;
    RecordingListener a("A", log);
    std::uint64_t seen_record_id = 0;
    a.action = [&](Bus&, const EventView& event) { seen_record_id = event.record_id; };
    REQUIRE_FALSE(bus.subscribe(a, key));

    bus.set_tick(5);
    Json payload = Json::object();
    payload.set("hp", 7);
    TriggerResult result = bus.trigger(key, Name("boss.custom"), payload, 0);
    REQUIRE_FALSE(result.error.has_value());
    CHECK(result.record_id == 1);
    // The listener received THE journal id of its cause.
    CHECK(seen_record_id == 1);

    // Zero-subscriber triggers journal too (subscribers: 0, empty payload
    // omitted — the record convention). An entity key spells itself out.
    const auto entity = fix.world.spawn();
    REQUIRE_FALSE(
        bus.trigger(EventKey::entity(entity), Name("ping"), Json::object(), 1).error.has_value());

    std::vector<Record> records = fix.finish();
    REQUIRE(records.size() == 2);
    CHECK(midday::journal::to_jsonl(records[0]) ==
          R"({"tick":5,"tier":"flight","kind":"event.trigger","cause_id":0,"id":1,)"
          R"("payload":{"event":"boss.custom","key":"alpha","payload":{"hp":7},"subscribers":1}})");
    CHECK(midday::journal::to_jsonl(records[1]) ==
          R"({"tick":5,"tier":"flight","kind":"event.trigger","cause_id":1,"id":2,)"
          R"("payload":{"event":"ping","key":"entity:0#0","subscribers":0}})");
}

TEST_CASE("bus.journal: cause-id chains — trigger -> effect -> effect walks back to the root") {
    BusFixture fix;
    Bus& bus = fix.bus();
    const EventKey alpha = EventKey::named(Name("alpha"));
    const EventKey beta = EventKey::named(Name("beta"));
    const EventKey gamma = EventKey::named(Name("gamma"));

    std::vector<std::string> log;
    RecordingListener first("first", log);
    RecordingListener second("second", log);
    first.action = [&](Bus& inner, const EventView& event) {
        // The effect cites its cause: the record id delivered with the event.
        REQUIRE_FALSE(inner.trigger(beta, Name("b.effect"), Json::object(), event.record_id)
                          .error.has_value());
    };
    second.action = [&](Bus& inner, const EventView& event) {
        REQUIRE_FALSE(inner.trigger(gamma, Name("c.effect"), Json::object(), event.record_id)
                          .error.has_value());
    };
    REQUIRE_FALSE(bus.subscribe(first, alpha));
    REQUIRE_FALSE(bus.subscribe(second, beta));

    REQUIRE_FALSE(bus.trigger(alpha, Name("a.root"), Json::object(), 0).error.has_value());

    std::vector<Record> records = fix.finish();
    REQUIRE(records.size() == 3);
    // Walk the chain in the journal and assert parentage, end to end.
    CHECK(records[0].kind == "event.trigger");
    CHECK(field(records[0].payload, "event").as_string() == "a.root");
    CHECK(records[0].cause_id == 0);
    CHECK(field(records[1].payload, "event").as_string() == "b.effect");
    CHECK(records[1].cause_id == records[0].id);
    CHECK(field(records[2].payload, "event").as_string() == "c.effect");
    CHECK(records[2].cause_id == records[1].id);
}

TEST_CASE("bus.journal: depth-33 refusal — 32 chained triggers, then a bus.cascade_depth record "
          "closing the chain") {
    BusFixture fix;
    Bus& bus = fix.bus();
    const EventKey key = EventKey::named(Name("chain"));

    std::vector<std::string> log;
    RecordingListener cascader("cascade", log);
    cascader.action = [&](Bus& inner, const EventView& event) {
        inner.trigger(key, Name("chain.step"), Json::object(), event.record_id);
    };
    REQUIRE_FALSE(bus.subscribe(cascader, key));
    REQUIRE_FALSE(bus.trigger(key, Name("chain.step"), Json::object(), 0).error.has_value());

    std::vector<Record> records = fix.finish();
    REQUIRE(records.size() == 33); // 32 triggers + 1 refusal
    for (std::size_t i = 0; i < 32; ++i) {
        CHECK(records[i].kind == "event.trigger");
        CHECK(records[i].id == i + 1);
        CHECK(records[i].cause_id == i); // level N caused by level N-1 (root: 0)
    }
    const Record& refusal = records[32];
    CHECK(refusal.kind == "bus.cascade_depth");
    CHECK(refusal.cause_id == records[31].id); // caused by the deepest trigger
    CHECK(field(refusal.payload, "event").as_string() == "chain.step");
    CHECK(field(refusal.payload, "depth").as_int() == 33);
    CHECK(field(refusal.payload, "cap").as_int() == 32);
}

TEST_CASE("bus.journal: a vocabulary trigger carries the D5 canonical envelope — bytes "
          "authoritative, projection decoded, dispatch normalized") {
    BusFixture fix;
    Bus& bus = fix.bus();
    const EventKey key = EventKey::named(Name("global"));

    // The listener sees the decoded normalized PROJECTION, never the
    // caller's raw spelling: -0 arrives as +0, schema declaration order.
    std::vector<std::string> log;
    RecordingListener a("A", log);
    Json seen;
    a.action = [&](Bus&, const EventView& event) { seen = event.payload; };
    REQUIRE_FALSE(bus.subscribe(a, key));

    Json payload = Json::object();
    payload.set("device", 0); // authored out of schema order, with a -0 float
    payload.set("action", "jump");
    payload.set("strength", -0.0);
    TriggerResult result = bus.trigger(key, Name("action.pressed"), payload, 0);
    REQUIRE_FALSE(result.error.has_value());
    CHECK(seen.dump() == R"({"action":"jump","strength":0,"device":0})");

    std::vector<Record> records = fix.finish();
    REQUIRE(records.size() == 1);
    const Record& record = records[0];
    // The envelope: codec + schema hash + AUTHORITATIVE lowercase-hex bytes
    // + the projection — and the projection is EXACTLY the bytes' decode.
    CHECK(field(record.payload, "payload_codec").as_string() == midday::reflect::kPayloadCodecName);
    const auto* entry = fix.registry.find_event(Name("action.pressed"));
    REQUIRE(entry != nullptr);
    CHECK(field(record.payload, "payload_schema").as_string() ==
          midday::base::hex64(entry->desc.compat_hash));
    const std::string& hex = field(record.payload, "payload_bytes").as_string();
    const auto bytes = midday::base::hex_to_bytes(hex);
    midday::reflect::DecodeResult decoded =
        midday::reflect::decode_payload(&entry->desc, unwrap(bytes));
    REQUIRE_FALSE(decoded.error.has_value());
    CHECK(field(record.payload, "payload").dump() == decoded.payload.dump());
    CHECK(field(record.payload, "payload").dump() ==
          R"({"action":"jump","strength":0,"device":0})");
}

TEST_CASE("bus.journal: a non-finite float refuses bus.payload_invalid WITH the field path") {
    BusFixture fix;
    Bus& bus = fix.bus();
    const EventKey key = EventKey::named(Name("global"));

    std::vector<std::string> log;
    RecordingListener a("A", log);
    REQUIRE_FALSE(bus.subscribe(a, key));

    Json payload = Json::object();
    payload.set("action", "jump");
    payload.set("strength", std::numeric_limits<double>::quiet_NaN());
    payload.set("device", 0);
    TriggerResult refused = bus.trigger(key, Name("action.pressed"), payload, 0);
    CHECK(code_of(refused.error) == "bus.payload_invalid");
    CHECK(log.empty()); // refused triggers dispatch nothing
    CHECK(field(unwrap(refused.error).details, "reason").as_string() == "non_finite");
    CHECK(field(unwrap(refused.error).details, "field").as_string() == "strength");

    std::vector<Record> records = fix.finish();
    REQUIRE(records.size() == 1);
    CHECK(records[0].kind == "bus.payload_invalid");
    // The refusal record's payload IS the error's details — one shape.
    CHECK(records[0].payload.dump() == unwrap(refused.error).details.dump());
}

TEST_CASE("bus.journal: an invalid payload journals its refusal, and dispatches nothing") {
    BusFixture fix;
    Bus& bus = fix.bus();
    const EventKey key = EventKey::named(Name("global"));

    std::vector<std::string> log;
    RecordingListener a("A", log);
    REQUIRE_FALSE(bus.subscribe(a, key));

    // A real root cause first (the journal's cause discipline: cite only
    // already-consumed ids), then the invalid trigger chained from it.
    TriggerResult root = bus.trigger(key, Name("boss.custom"), Json::object(), 0);
    REQUIRE_FALSE(root.error.has_value());
    log.clear();

    Json bad = Json::object();
    bad.set("action", "jump"); // strength + device missing
    TriggerResult refused = bus.trigger(key, Name("action.pressed"), bad, root.record_id);
    CHECK(code_of(refused.error) == "bus.payload_invalid");
    CHECK(log.empty());

    std::vector<Record> records = fix.finish();
    REQUIRE(records.size() == 2);
    CHECK(records[1].kind == "bus.payload_invalid");
    CHECK(records[1].cause_id == root.record_id);
    CHECK(field(records[1].payload, "event").as_string() == "action.pressed");
    CHECK(field(records[1].payload, "reason").as_string() == "missing_field");
    // The refusal record's payload IS the error's details — one shape.
    CHECK(records[1].payload.dump() == unwrap(refused.error).details.dump());
}

TEST_CASE("bus.journal: no record, no dispatch — a poisoned journal refuses the trigger") {
    BusFixture fix;
    Bus& bus = fix.bus();
    const EventKey key = EventKey::named(Name("alpha"));

    std::vector<std::string> log;
    RecordingListener a("A", log);
    REQUIRE_FALSE(bus.subscribe(a, key));

    // Poison the writer: citing a future cause is journal.cause_unknown.
    CHECK(fix.writer().record(0, midday::journal::Tier::Flight, "poison", 999, Json::object()) ==
          0);
    REQUIRE(fix.writer().status().has_value());

    TriggerResult refused = bus.trigger(key, Name("hit"), Json::object(), 0);
    CHECK(code_of(refused.error) == "bus.journal_refused");
    CHECK(refused.record_id == 0);
    CHECK(refused.delivered == 0);
    CHECK(log.empty()); // unjournaled effects do not exist
    CHECK(field(unwrap(refused.error).details, "journal").as_string() == "journal.cause_unknown");
}

TEST_CASE("bus.determinism: two independently driven buses journal identical records") {
    auto drive = [](BusFixture& fix) {
        Bus& bus = fix.bus();
        const EventKey alpha = EventKey::named(Name("alpha"));
        const EventKey chain = EventKey::named(Name("chain"));

        std::vector<std::string> log;
        RecordingListener a("A", log);
        RecordingListener b("B", log);
        RecordingListener cascader("c", log);
        cascader.action = [&](Bus& inner, const EventView& event) {
            inner.trigger(chain, Name("chain.step"), Json::object(), event.record_id);
        };
        REQUIRE_FALSE(bus.subscribe(a, alpha));
        REQUIRE_FALSE(bus.subscribe(b, alpha));
        REQUIRE_FALSE(bus.subscribe(cascader, chain));

        bus.set_tick(1);
        Json payload = Json::object();
        payload.set("value", 3);
        REQUIRE_FALSE(bus.trigger(alpha, Name("boss.custom"), payload, 0).error.has_value());
        REQUIRE_FALSE(bus.unsubscribe(a, alpha));
        bus.set_tick(2);
        REQUIRE_FALSE(bus.trigger(alpha, Name("boss.custom"), payload, 1).error.has_value());
        bus.set_tick(3);
        REQUIRE_FALSE(bus.trigger(chain, Name("chain.step"), Json::object(), 0).error.has_value());
        return fix.finish();
    };

    BusFixture run_a;
    BusFixture run_b;
    std::vector<Record> a = drive(run_a);
    std::vector<Record> b = drive(run_b);
    REQUIRE(a.size() == b.size());
    for (std::size_t i = 0; i < a.size(); ++i)
        CHECK(midday::journal::to_jsonl(a[i]) == midday::journal::to_jsonl(b[i]));
}
