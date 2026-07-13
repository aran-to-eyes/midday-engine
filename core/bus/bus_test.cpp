// bus.dispatch / bus.keys / bus.defer / bus.entity / bus.validate / bus.errors
// — the event bus's dispatch semantics: registration order (incl. after
// unsubscribe/resubscribe), key isolation, cascade depth cap with completed
// outer levels, deferred subscribe/unsubscribe during dispatch, the ECS
// component adaptor's lifecycle (inactive/pending/stale), and typed payload
// validation (core/bus/bus.h). Journal record shapes live in
// bus_journal_test.cpp.

#include "core/base/json.h"
#include "core/base/name.h"
#include "core/bus/bus.h"
#include "core/bus/entity_listener.h"
#include "core/bus/test_support.h"
#include "core/ecs/world.h"
#include "testkit/doctest.h"

#include <cstdint>
#include <optional>
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
using midday::ecs::EntityRef;
using midday::reflect::ClassDesc;
using midday::testkit::unwrap;

namespace {

// A component listener for the ECS bridge tests (duck-typed: no vtable).
struct Counter {
    int hits = 0;
    std::uint64_t last_cause = 0;

    void on_event(Bus& /*bus*/, const EventView& event) {
        ++hits;
        last_cause = event.record_id;
    }
};

ClassDesc component_class(const char* name) {
    ClassDesc cls;
    cls.name = Name(name);
    return cls;
}

Json payload_hp(int hp) {
    Json payload = Json::object();
    payload.set("hp", hp);
    return payload;
}

} // namespace

TEST_CASE("bus.dispatch: registration order, including after unsubscribe/resubscribe") {
    BusFixture fix;
    Bus& bus = fix.bus();
    const EventKey key = EventKey::named(Name("alpha"));

    std::vector<std::string> log;
    RecordingListener a("A", log);
    RecordingListener b("B", log);
    RecordingListener c("C", log);
    REQUIRE_FALSE(bus.subscribe(a, key));
    REQUIRE_FALSE(bus.subscribe(b, key));
    REQUIRE_FALSE(bus.subscribe(c, key));
    CHECK(bus.subscriber_count(key) == 3);

    TriggerResult first = bus.trigger(key, Name("boss.custom"), Json::object(), 0);
    REQUIRE_FALSE(first.error.has_value());
    CHECK(first.record_id == 1);
    CHECK(first.delivered == 3);
    CHECK(log == std::vector<std::string>{"A:boss.custom", "B:boss.custom", "C:boss.custom"});

    // Unsubscribe the middle listener: survivors keep their relative order.
    REQUIRE_FALSE(bus.unsubscribe(b, key));
    log.clear();
    REQUIRE_FALSE(bus.trigger(key, Name("boss.custom"), Json::object(), 0).error.has_value());
    CHECK(log == std::vector<std::string>{"A:boss.custom", "C:boss.custom"});

    // Resubscribing is a FRESH registration: B now dispatches last.
    REQUIRE_FALSE(bus.subscribe(b, key));
    log.clear();
    REQUIRE_FALSE(bus.trigger(key, Name("boss.custom"), Json::object(), 0).error.has_value());
    CHECK(log == std::vector<std::string>{"A:boss.custom", "C:boss.custom", "B:boss.custom"});
}

TEST_CASE("bus.keys: isolation — same event name never crosses channels") {
    BusFixture fix;
    Bus& bus = fix.bus();

    std::vector<std::string> log;
    RecordingListener alpha("alpha", log);
    RecordingListener beta("beta", log);
    REQUIRE_FALSE(bus.subscribe(alpha, EventKey::named(Name("alpha"))));
    REQUIRE_FALSE(bus.subscribe(beta, EventKey::named(Name("beta"))));

    // Entity-private channels: one listener per entity key.
    const EntityRef e1 = fix.world.spawn();
    const EntityRef e2 = fix.world.spawn();
    RecordingListener ent1("e1", log);
    RecordingListener ent2("e2", log);
    REQUIRE_FALSE(bus.subscribe(ent1, EventKey::entity(e1)));
    REQUIRE_FALSE(bus.subscribe(ent2, EventKey::entity(e2)));

    TriggerResult hit = bus.trigger(EventKey::named(Name("alpha")), Name("hit"), Json::object(), 0);
    REQUIRE_FALSE(hit.error.has_value());
    CHECK(hit.delivered == 1);
    CHECK(log == std::vector<std::string>{"alpha:hit"});

    log.clear();
    REQUIRE_FALSE(
        bus.trigger(EventKey::entity(e2), Name("hit"), Json::object(), 0).error.has_value());
    CHECK(log == std::vector<std::string>{"e2:hit"});

    // A channel nobody holds the key for: journaled, delivered to no one.
    log.clear();
    TriggerResult nobody =
        bus.trigger(EventKey::named(Name("gamma")), Name("hit"), Json::object(), 0);
    REQUIRE_FALSE(nobody.error.has_value());
    CHECK(nobody.delivered == 0);
    CHECK(log.empty());
}

TEST_CASE("bus.dispatch: cascade depth — 32 levels dispatch, the 33rd is refused, outer levels "
          "complete") {
    BusFixture fix;
    Bus& bus = fix.bus();
    const EventKey key = EventKey::named(Name("chain"));

    std::vector<std::string> log;
    std::vector<TriggerResult> refused;
    RecordingListener cascader("cascade", log);
    cascader.action = [&](Bus& inner, const EventView& event) {
        TriggerResult next =
            inner.trigger(key, Name("chain.step"), Json::object(), event.record_id);
        if (next.error.has_value())
            refused.push_back(std::move(next));
    };
    RecordingListener after("after", log);
    REQUIRE_FALSE(bus.subscribe(cascader, key));
    REQUIRE_FALSE(bus.subscribe(after, key));

    TriggerResult top = bus.trigger(key, Name("chain.step"), Json::object(), 0);
    REQUIRE_FALSE(top.error.has_value());
    CHECK_FALSE(bus.dispatching());

    // 32 accepted triggers (levels 1..32); exactly one refusal (level 33).
    CHECK(bus.stats().triggers == 32);
    REQUIRE(refused.size() == 1);
    CHECK(refused[0].record_id == 0);
    CHECK(code_of(refused[0].error) == "bus.cascade_depth");
    const Json& details = unwrap(refused[0].error).details;
    CHECK(field(details, "depth").as_int() == 33);
    CHECK(field(details, "cap").as_int() == 32);

    // NO unwinding: after the refusal returned to level 32's listener, the
    // "after" listener still ran at every level — 32 times.
    int cascades = 0;
    int afters = 0;
    for (const std::string& line : log) {
        if (line == "cascade:chain.step")
            ++cascades;
        if (line == "after:chain.step")
            ++afters;
    }
    CHECK(cascades == 32);
    CHECK(afters == 32);
}

TEST_CASE("bus.defer: subscribe during dispatch lands at dispatch end, in queue order") {
    BusFixture fix;
    Bus& bus = fix.bus();
    const EventKey key = EventKey::named(Name("alpha"));

    std::vector<std::string> log;
    RecordingListener late("late", log);
    RecordingListener seed("seed", log);
    bool planted = false;
    seed.action = [&](Bus& inner, const EventView&) {
        if (!planted) {
            planted = true;
            REQUIRE_FALSE(inner.subscribe(late, key)); // deferred: queued, no error
        }
    };
    REQUIRE_FALSE(bus.subscribe(seed, key));

    // The cascade that creates a subscriber never delivers to it.
    TriggerResult first = bus.trigger(key, Name("hit"), Json::object(), 0);
    CHECK(first.delivered == 1);
    CHECK(log == std::vector<std::string>{"seed:hit"});
    CHECK(bus.subscriber_count(key) == 2); // applied at dispatch end

    log.clear();
    TriggerResult second = bus.trigger(key, Name("hit"), Json::object(), 0);
    CHECK(second.delivered == 2);
    CHECK(log == std::vector<std::string>{"seed:hit", "late:hit"});
}

TEST_CASE("bus.defer: unsubscribe during dispatch — the listener still hears the cascade out") {
    BusFixture fix;
    Bus& bus = fix.bus();
    const EventKey key = EventKey::named(Name("alpha"));

    std::vector<std::string> log;
    RecordingListener victim("victim", log);
    RecordingListener remover("remover", log);
    remover.action = [&](Bus& inner, const EventView&) {
        REQUIRE_FALSE(inner.unsubscribe(victim, key));  // deferred
        REQUIRE_FALSE(inner.unsubscribe(remover, key)); // self-unsubscribe, deferred too
    };
    REQUIRE_FALSE(bus.subscribe(remover, key));
    REQUIRE_FALSE(bus.subscribe(victim, key));

    // Both still receive THIS dispatch (removal applies at dispatch end).
    TriggerResult first = bus.trigger(key, Name("hit"), Json::object(), 0);
    CHECK(first.delivered == 2);
    CHECK(log == std::vector<std::string>{"remover:hit", "victim:hit"});
    CHECK(bus.subscriber_count(key) == 0);

    log.clear();
    TriggerResult second = bus.trigger(key, Name("hit"), Json::object(), 0);
    CHECK(second.delivered == 0);
    CHECK(log.empty());
}

TEST_CASE("bus.entity: component listeners — delivery, inactive skip, pending wake, stale "
          "auto-unsubscribe") {
    BusFixture fix;
    Bus& bus = fix.bus();
    fix.world.register_component<Counter>(component_class("BusTestCounter"));

    // Alive + active component: delivered, cause id = the trigger record.
    const EntityRef e = fix.world.spawn();
    REQUIRE_FALSE(fix.world.emplace(e, Counter{}));
    const EventKey key = EventKey::entity(e);
    REQUIRE_FALSE(midday::bus::subscribe_component<Counter>(bus, key, e));
    TriggerResult first = bus.trigger(key, Name("hit"), Json::object(), 0);
    CHECK(first.delivered == 1);
    CHECK(fix.world.try_get<Counter>(e)->hits == 1);
    CHECK(fix.world.try_get<Counter>(e)->last_cause == first.record_id);

    // Dormant component (state-toggled): skipped, still subscribed.
    REQUIRE_FALSE(fix.world.set_active<Counter>(e, false));
    CHECK(bus.trigger(key, Name("hit"), Json::object(), 0).delivered == 0);
    CHECK(bus.stats().skipped_inactive == 1);
    CHECK(bus.subscriber_count(key) == 1);
    REQUIRE_FALSE(fix.world.set_active<Counter>(e, true));
    CHECK(bus.trigger(key, Name("hit"), Json::object(), 0).delivered == 1);

    // Pending entity (queue_spawn window): skipped, wakes after the flush.
    const EntityRef pending = fix.world.queue_spawn();
    const EventKey pending_key = EventKey::entity(pending);
    REQUIRE_FALSE(midday::bus::subscribe_component<Counter>(bus, pending_key, pending));
    CHECK(bus.trigger(pending_key, Name("hit"), Json::object(), 0).delivered == 0);
    CHECK(bus.stats().skipped_pending == 1);
    CHECK(bus.subscriber_count(pending_key) == 1);
    REQUIRE_FALSE(fix.world.flush_structural());
    REQUIRE_FALSE(fix.world.emplace(pending, Counter{}));
    CHECK(bus.trigger(pending_key, Name("hit"), Json::object(), 0).delivered == 1);

    // Despawn: the stale subscription is dropped at the next dispatch,
    // without ever touching component memory.
    REQUIRE_FALSE(fix.world.despawn(e));
    CHECK(bus.trigger(key, Name("hit"), Json::object(), 0).delivered == 0);
    CHECK(bus.stats().auto_unsubscribed == 1);
    CHECK(bus.subscriber_count(key) == 0);
}

TEST_CASE("bus.entity: a despawn earlier in the SAME dispatch stales later deliveries") {
    BusFixture fix;
    Bus& bus = fix.bus();
    fix.world.register_component<Counter>(component_class("BusTestCounter2"));

    const EntityRef e = fix.world.spawn();
    REQUIRE_FALSE(fix.world.emplace(e, Counter{}));
    const EventKey key = EventKey::named(Name("squad"));

    std::vector<std::string> log;
    RecordingListener killer("killer", log);
    killer.action = [&](Bus&, const EventView&) { REQUIRE_FALSE(fix.world.despawn(e)); };
    REQUIRE_FALSE(bus.subscribe(killer, key));
    REQUIRE_FALSE(midday::bus::subscribe_component<Counter>(bus, key, e));

    // Generation checks happen per DELIVERY: the entity died mid-dispatch,
    // so its component never hears the event that killed it.
    TriggerResult result = bus.trigger(key, Name("wipe"), Json::object(), 0);
    CHECK(result.delivered == 1);
    CHECK(bus.stats().auto_unsubscribed == 1);
    CHECK(bus.subscriber_count(key) == 1);
}

TEST_CASE("bus.entity_listener: generation-gated listener pointers — order, pending wake, stale "
          "auto-unsubscribe (M2 0B)") {
    BusFixture fix;
    Bus& bus = fix.bus();

    // Delivery interleaves with the other flavors in REGISTRATION order.
    const EntityRef e = fix.world.spawn();
    const EventKey key = EventKey::entity(e);
    std::vector<std::string> log;
    RecordingListener plain("plain", log);
    RecordingListener bound("bound", log);
    REQUIRE_FALSE(bus.subscribe(plain, key));
    REQUIRE_FALSE(bus.subscribe_entity_listener(key, e, bound));
    TriggerResult first = bus.trigger(key, Name("hit"), Json::object(), 0);
    CHECK(first.delivered == 2);
    CHECK(log == std::vector<std::string>{"plain:hit", "bound:hit"});

    // Identity is (key, entity, listener): a duplicate refuses, a SECOND
    // listener on the same entity is its own subscription.
    CHECK(code_of(bus.subscribe_entity_listener(key, e, bound)) == "bus.duplicate_subscription");
    RecordingListener sibling("sibling", log);
    REQUIRE_FALSE(bus.subscribe_entity_listener(key, e, sibling));
    CHECK(bus.subscriber_count(key) == 3);
    REQUIRE_FALSE(bus.unsubscribe_entity_listener(key, e, sibling));
    CHECK(bus.subscriber_count(key) == 2);

    // Pending entity (queue_spawn window): skipped, kept, wakes at flush.
    const EntityRef pending = fix.world.queue_spawn();
    const EventKey pending_key = EventKey::entity(pending);
    RecordingListener sleeper("sleeper", log);
    REQUIRE_FALSE(bus.subscribe_entity_listener(pending_key, pending, sleeper));
    CHECK(bus.trigger(pending_key, Name("hit"), Json::object(), 0).delivered == 0);
    CHECK(bus.stats().skipped_pending == 1);
    CHECK(bus.subscriber_count(pending_key) == 1);
    REQUIRE_FALSE(fix.world.flush_structural());
    CHECK(bus.trigger(pending_key, Name("hit"), Json::object(), 0).delivered == 1);

    // Stale generation: auto-unsubscribed WITHOUT invoking the bound
    // listener — the PLAIN listener on the same channel keeps hearing (it
    // opted out of the generation gate by construction).
    log.clear();
    REQUIRE_FALSE(fix.world.despawn(e));
    CHECK(bus.trigger(key, Name("hit"), Json::object(), 0).delivered == 1);
    CHECK(log == std::vector<std::string>{"plain:hit"});
    CHECK(bus.stats().auto_unsubscribed == 1);
    CHECK(bus.subscriber_count(key) == 1);

    // A dead-entity subscribe refuses up front, exactly like the thunk flavor.
    CHECK(code_of(bus.subscribe_entity_listener(key, e, bound)) == "ecs.stale_handle");
}

TEST_CASE("bus.validate: vocabulary events check their typed payload schema") {
    BusFixture fix;
    Bus& bus = fix.bus();
    const EventKey key = EventKey::named(Name("global"));

    Json good = Json::object();
    good.set("action", "jump");
    good.set("strength", 1.0);
    good.set("device", 0);
    REQUIRE_FALSE(bus.trigger(key, Name("action.pressed"), good, 0).error.has_value());

    // Missing field.
    Json missing = Json::object();
    missing.set("action", "jump");
    missing.set("strength", 1.0);
    TriggerResult refused = bus.trigger(key, Name("action.pressed"), missing, 0);
    CHECK(code_of(refused.error) == "bus.payload_invalid");
    CHECK(refused.record_id == 0);
    CHECK(field(unwrap(refused.error).details, "reason").as_string() == "missing_field");
    CHECK(field(unwrap(refused.error).details, "field").as_string() == "device");

    // Unknown field: agents deserve refusal over silent tolerance.
    Json extra = good;
    extra.set("volume", 11);
    refused = bus.trigger(key, Name("action.pressed"), extra, 0);
    CHECK(code_of(refused.error) == "bus.payload_invalid");
    CHECK(field(unwrap(refused.error).details, "reason").as_string() == "unknown_field");
    CHECK(field(unwrap(refused.error).details, "field").as_string() == "volume");

    // Mistyped field.
    Json mistyped = good;
    mistyped.set("action", 7);
    refused = bus.trigger(key, Name("action.pressed"), mistyped, 0);
    CHECK(code_of(refused.error) == "bus.payload_invalid");
    CHECK(field(unwrap(refused.error).details, "reason").as_string() == "field_type");
    CHECK(field(unwrap(refused.error).details, "field").as_string() == "action");
    CHECK(field(unwrap(refused.error).details, "expected").as_string() == "name");

    // Non-object payload for a vocabulary event.
    refused = bus.trigger(key, Name("action.pressed"), Json("zap"), 0);
    CHECK(code_of(refused.error) == "bus.payload_invalid");
    CHECK(field(unwrap(refused.error).details, "reason").as_string() == "not_object");

    // entity_ref fields use the RUNTIME wire shape: EntityRef::to_bits()
    // (the authoring string form is a loader concern — D-BUILD-046).
    const EntityRef e = fix.world.spawn();
    Json entered = Json::object();
    entered.set("trigger", Json(static_cast<std::int64_t>(e.to_bits())));
    entered.set("other", Json(static_cast<std::int64_t>(e.to_bits())));
    REQUIRE_FALSE(bus.trigger(key, Name("trigger.entered"), entered, 0).error.has_value());
    entered.set("other", Json(std::int64_t{-3}));
    CHECK(code_of(bus.trigger(key, Name("trigger.entered"), entered, 0).error) ==
          "bus.payload_invalid");

    // Unregistered event names pass through with any payload shape.
    REQUIRE_FALSE(bus.trigger(key, Name("boss.custom"), payload_hp(7), 0).error.has_value());
}

TEST_CASE("bus.errors: null keys, duplicate subscribe, no-op unsubscribe") {
    BusFixture fix;
    Bus& bus = fix.bus();

    std::vector<std::string> log;
    RecordingListener a("A", log);

    // The empty named key is a null capability: nothing speaks, nothing hears.
    CHECK(code_of(bus.subscribe(a, EventKey())) == "bus.null_key");
    TriggerResult refused = bus.trigger(EventKey(), Name("hit"), Json::object(), 0);
    CHECK(code_of(refused.error) == "bus.null_key");
    CHECK(refused.record_id == 0);

    const EventKey key = EventKey::named(Name("alpha"));
    REQUIRE_FALSE(bus.subscribe(a, key));
    CHECK(code_of(bus.subscribe(a, key)) == "bus.duplicate_subscription");

    // Unsubscribing what is not subscribed: counted no-op (D-BUILD-047).
    RecordingListener stranger("S", log);
    REQUIRE_FALSE(bus.unsubscribe(stranger, key));
    CHECK(bus.stats().noop_unsubscribes == 1);

    // Deferred duplicate subscribe: dropped at apply, counted.
    bool tried = false;
    a.action = [&](Bus& inner, const EventView&) {
        if (!tried) {
            tried = true;
            REQUIRE_FALSE(inner.subscribe(a, key)); // queued; found duplicate at apply
        }
    };
    REQUIRE_FALSE(bus.trigger(key, Name("hit"), Json::object(), 0).error.has_value());
    CHECK(bus.stats().dropped_subscribes == 1);
    CHECK(bus.subscriber_count(key) == 1);
}
