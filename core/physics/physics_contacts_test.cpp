// core/physics/physics_contacts_test.cpp — physics.min.contacts: the
// MILESTONE_0 item-17 exit test. Contacts collected DURING the Jolt step
// trigger AFTER it on the bus, in phase-6 order sorted by body-pair id;
// every trigger's cause is that tick's phase-6 marker. Proven by live bus
// delivery AND a journal walk over the recorded run.

#include "core/journal/record.h"
#include "core/physics/physics_server.h"
#include "core/physics/test_support.h"

#include <cstdint>
#include <string>
#include <vector>

namespace midday::physics {
namespace {

using test::bits_of;
using test::ContactLog;
using test::PhysicsFixture;

TEST_CASE("physics.min.contacts: contact.began on the bus, phase-6 order by body-pair id") {
    PhysicsFixture fixture;
    PhysicsServer& server = *fixture.server;
    bus::Bus& bus = fixture.sim.bus();

    // Ground (body 0), then three boxes (bodies 1..3, ascending body id in
    // creation order) hovering just above it — all three land on the SAME
    // tick, so one phase-6 dispatch carries three pairs to sort.
    const ecs::EntityRef ground = fixture.adopted_entity();
    REQUIRE_FALSE(server.create_ground_plane(ground, 0.0F).is_null());
    const ecs::EntityRef box1 = fixture.drop_box({-3.0F, 0.6F, 0.0F});
    const ecs::EntityRef box2 = fixture.drop_box({0.0F, 0.6F, 0.0F});
    const ecs::EntityRef box3 = fixture.drop_box({3.0F, 0.6F, 0.0F});

    ContactLog ground_log;
    ContactLog box2_log;
    REQUIRE_FALSE(bus.subscribe(ground_log, bus::EventKey::entity(ground)).has_value());
    REQUIRE_FALSE(bus.subscribe(box2_log, bus::EventKey::entity(box2)).has_value());

    REQUIRE_FALSE(fixture.sim.loop().tick(30).has_value());

    // Live delivery: the ground heard all three pairs in body-id order; the
    // registered payload schema validated every trigger (zero refusals).
    REQUIRE(ground_log.entries.size() >= 3);
    const std::string g = std::to_string(bits_of(ground));
    CHECK(ground_log.entries[0] == "contact.began:" + g + "->" + std::to_string(bits_of(box1)));
    CHECK(ground_log.entries[1] == "contact.began:" + g + "->" + std::to_string(bits_of(box2)));
    CHECK(ground_log.entries[2] == "contact.began:" + g + "->" + std::to_string(bits_of(box3)));
    REQUIRE_FALSE(box2_log.entries.empty());
    CHECK(box2_log.entries[0] == "contact.began:" + std::to_string(bits_of(box2)) + "->" + g);
    CHECK(server.stats().trigger_refusals == 0);
    CHECK(server.stats().contacts_began >= 3);

    // Separation: flip gravity, the boxes lift off, contact.ended follows.
    server.set_gravity({0.0F, 9.81F, 0.0F});
    REQUIRE_FALSE(fixture.sim.loop().tick(30).has_value());
    CHECK(server.stats().contacts_ended >= 3);

    REQUIRE_FALSE(bus.unsubscribe(ground_log, bus::EventKey::entity(ground)).has_value());
    REQUIRE_FALSE(bus.unsubscribe(box2_log, bus::EventKey::entity(box2)).has_value());

    // ---- journal walk: phase-6 marker -> sorted triggers, cause ids ------
    const std::vector<journal::Record> records = fixture.sim.finish();

    // Every physics phase marker by record id.
    std::vector<std::uint64_t> physics_markers;
    for (const journal::Record& record : records)
        if (record.kind == "tick.phase" &&
            test::field(record.payload, "phase").as_string() == "physics")
            physics_markers.push_back(record.id);

    // The contact triggers, in journal (= dispatch) order.
    struct Trigger {
        std::uint64_t id = 0;
        std::uint64_t cause = 0;
        std::uint64_t tick = 0;
        std::int64_t self = 0;
        std::int64_t other = 0;
        bool began = false;
    };

    std::vector<Trigger> triggers;
    for (const journal::Record& record : records) {
        if (record.kind != "event.trigger")
            continue;
        const std::string& event = test::field(record.payload, "event").as_string();
        if (event != "contact.began" && event != "contact.ended")
            continue;
        const base::Json& payload = test::field(record.payload, "payload");
        triggers.push_back({record.id,
                            record.cause_id,
                            record.tick,
                            test::field(payload, "self").as_int(),
                            test::field(payload, "other").as_int(),
                            event == "contact.began"});
    }
    REQUIRE(triggers.size() >= 6); // three pairs x two channels, began alone

    // (1) Every contact trigger cites a phase-6 marker as cause, and the
    // marker precedes the trigger in the journal (record-before-effect).
    for (const Trigger& trigger : triggers) {
        bool cites_physics_marker = false;
        for (std::uint64_t marker : physics_markers)
            cites_physics_marker = cites_physics_marker || trigger.cause == marker;
        CHECK(cites_physics_marker);
        CHECK(trigger.cause < trigger.id);
    }

    // (2) The landing tick's began triggers are EXACTLY the three pairs,
    // sorted by body-pair id: (ground,box1),(ground,box2),(ground,box3),
    // lower body id's channel first within each pair.
    const std::uint64_t landing_tick = triggers.front().tick;
    std::vector<std::string> landing;
    for (const Trigger& trigger : triggers)
        if (trigger.began && trigger.tick == landing_tick)
            landing.push_back(std::to_string(trigger.self) + "->" + std::to_string(trigger.other));
    const std::string b1 = std::to_string(bits_of(box1));
    const std::string b2 = std::to_string(bits_of(box2));
    const std::string b3 = std::to_string(bits_of(box3));
    REQUIRE(landing.size() == 6);
    CHECK(landing[0] == g + "->" + b1);
    CHECK(landing[1] == b1 + "->" + g);
    CHECK(landing[2] == g + "->" + b2);
    CHECK(landing[3] == b2 + "->" + g);
    CHECK(landing[4] == g + "->" + b3);
    CHECK(landing[5] == b3 + "->" + g);

    // (3) contact.ended made it to the journal too, same causal shape.
    bool any_ended = false;
    for (const Trigger& trigger : triggers)
        any_ended = any_ended || !trigger.began;
    CHECK(any_ended);
}

} // namespace
} // namespace midday::physics
