// input.inject — the public synthetic injection API (EXIT-TEST #1: a
// synthetic input at tick 42 triggers the action event at tick 42 AND is
// journaled). Rides the tick loop's EXISTING inject_input seat — no second
// injection path (core/tick/tick_loop.h).

#include "core/base/json.h"
#include "core/input/inject.h"
#include "core/journal/record.h"
#include "core/loader/loader.h"
#include "testkit/doctest.h"
#include "testkit/sim_fixture.h"

#include <utility>
#include <vector>

using midday::base::Json;
using midday::input::ActionInjector;
using midday::input::RawEdge;
using midday::input::RawInput;
using midday::journal::Record;
using midday::loader::ActionDesc;
using midday::loader::ActionMapDecl;
using midday::loader::BindingDesc;
using midday::loader::DeviceKind;
using midday::testkit::code_of;
using midday::testkit::field;

namespace {

ActionMapDecl jump_map() {
    ActionMapDecl map;
    ActionDesc jump;
    jump.name = "jump";
    jump.bindings.push_back(BindingDesc{DeviceKind::kKeyboard, "space"});
    map.actions.push_back(std::move(jump));
    return map;
}

} // namespace

TEST_CASE("input.inject: EXIT-TEST #1 — a synthetic input at tick 42 triggers "
          "action.pressed at tick 42, journaled") {
    midday::testkit::SimFixture fix;
    REQUIRE_FALSE(fix.loop().tick(41).has_value());
    REQUIRE(fix.loop().current_tick() == 41);

    ActionMapDecl map = jump_map();
    ActionInjector injector(fix.loop(), map);

    RawInput raw;
    raw.device = DeviceKind::kKeyboard;
    raw.control = "space";
    raw.edge = RawEdge::kPressed;
    raw.strength = 1.0f;
    REQUIRE_FALSE(injector.inject_at(42, raw).has_value());
    CHECK(fix.loop().pending_input_count() == 1); // queued, not yet delivered

    REQUIRE_FALSE(fix.loop().tick().has_value()); // steps to tick 42, drains the input phase
    REQUIRE(fix.loop().current_tick() == 42);

    std::vector<Record> records = fix.finish();
    bool found = false;
    for (const Record& record : records) {
        if (record.kind != "event.trigger")
            continue;
        if (field(record.payload, "event").as_string() != "action.pressed")
            continue;
        found = true;
        CHECK(record.tick == 42);    // journaled AT tick 42, not 41 or 43
        CHECK(record.cause_id == 0); // a root record: it entered from outside the sim
        const Json& inner = field(record.payload, "payload");
        CHECK(field(inner, "action").as_string() == "jump");
        CHECK(field(inner, "strength").as_double() == doctest::Approx(1.0));
        CHECK(field(inner, "device").as_int() == 0);
    }
    CHECK(found);
}

TEST_CASE("input.inject: inject_at refuses a miscounted target tick") {
    midday::testkit::SimFixture fix;
    ActionMapDecl map = jump_map();
    ActionInjector injector(fix.loop(), map);
    RawInput raw;
    raw.control = "space";

    // Before the first tick, current_tick() == 0: the only valid next input
    // phase is tick 1 — anything else refuses loudly instead of drifting.
    CHECK(code_of(injector.inject_at(5, raw)) == "input.tick_mismatch");
    CHECK(fix.loop().pending_input_count() == 0); // the mismatched call queued nothing
    CHECK(code_of(injector.inject_at(1, raw)) == "<none>");
    CHECK(fix.loop().pending_input_count() == 1);
}

TEST_CASE("input.inject: an unbound raw control is a legitimate no-op") {
    midday::testkit::SimFixture fix;
    ActionMapDecl map = jump_map();
    ActionInjector injector(fix.loop(), map);
    RawInput raw;
    raw.device = DeviceKind::kKeyboard;
    raw.control = "unbound_key";
    REQUIRE_FALSE(injector.inject(raw).has_value());
    CHECK(fix.loop().pending_input_count() == 0);
}

TEST_CASE("input.inject: a released edge journals action.released with no strength field") {
    midday::testkit::SimFixture fix;
    ActionMapDecl map = jump_map();
    ActionInjector injector(fix.loop(), map);
    RawInput raw;
    raw.device = DeviceKind::kKeyboard;
    raw.control = "space";
    raw.edge = RawEdge::kReleased;
    REQUIRE_FALSE(injector.inject(raw).has_value());
    REQUIRE_FALSE(fix.loop().tick().has_value());

    std::vector<Record> records = fix.finish();
    bool found = false;
    for (const Record& record : records) {
        if (record.kind != "event.trigger" ||
            field(record.payload, "event").as_string() != "action.released")
            continue;
        found = true;
        const Json& inner = field(record.payload, "payload");
        CHECK(field(inner, "action").as_string() == "jump");
        CHECK(inner.find("strength") == nullptr); // action.released carries NO strength field
    }
    CHECK(found);
}
