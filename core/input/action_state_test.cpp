// input.vector / input.action_state — the Godot-precedent get_vector
// primitive (EXIT-TEST #2: a numeric fixture with hand-computed expected
// values) plus the action-strength cache that drives it end-to-end off real
// action.pressed/action.released bus triggers.

#include "core/base/json.h"
#include "core/base/name.h"
#include "core/bus/bus.h"
#include "core/input/action_state.h"
#include "core/loader/loader.h"
#include "testkit/doctest.h"
#include "testkit/sim_fixture.h"

using midday::base::Json;
using midday::base::Name;
using midday::bus::EventKey;
using midday::input::ActionState;
using midday::input::combine_vector;
using midday::loader::StickDesc;
using midday::math::Vec2;

TEST_CASE("input.vector: combine_vector matches hand-computed Godot-formula values") {
    // Diagonal, no deadzone bite: renormalizes to unit length — WASD's
    // 8-directional diagonal case needs no separate "digital normalize"
    // flag, since length > 1 triggers the SAME branch an analog stick uses.
    const Vec2 diagonal = combine_vector(0.0f, 1.0f, 0.0f, 1.0f, 0.2f);
    CHECK(diagonal.x == doctest::Approx(0.70710678f));
    CHECK(diagonal.y == doctest::Approx(0.70710678f));

    // Inside the deadzone (raw length sqrt(0.02) ~= 0.1414 < 0.2): zero.
    const Vec2 dead = combine_vector(0.0f, 0.1f, 0.0f, 0.1f, 0.2f);
    CHECK(dead.x == doctest::Approx(0.0f));
    CHECK(dead.y == doctest::Approx(0.0f));

    // Mid-range inverse-lerp remap: raw (0.3, 0.4), length 0.5, deadzone
    // 0.2 -> scale (0.5-0.2)/(1-0.2)/0.5 = 0.75 -> (0.225, 0.3).
    const Vec2 mid = combine_vector(0.0f, 0.3f, 0.0f, 0.4f, 0.2f);
    CHECK(mid.x == doctest::Approx(0.225f));
    CHECK(mid.y == doctest::Approx(0.3f));

    // Exactly at the unit boundary (raw length 1.0): scale collapses to 1,
    // the identity.
    const Vec2 boundary = combine_vector(0.0f, 0.6f, 0.0f, 0.8f, 0.2f);
    CHECK(boundary.x == doctest::Approx(0.6f));
    CHECK(boundary.y == doctest::Approx(0.8f));

    // Beyond unit length (an analog stick reporting an out-of-range raw
    // value): clamps to the unit vector in the same direction.
    const Vec2 over = combine_vector(0.0f, 2.0f, 0.0f, 0.0f, 0.2f);
    CHECK(over.x == doctest::Approx(1.0f));
    CHECK(over.y == doctest::Approx(0.0f));
}

TEST_CASE("input.action_state: strength tracks action.pressed/released; never-pressed is zero") {
    midday::testkit::SimFixture fix;
    ActionState state;
    const EventKey key = EventKey::named(Name("global"));
    REQUIRE_FALSE(fix.bus().subscribe(state, key).has_value());

    CHECK(state.strength(Name("jump")) == doctest::Approx(0.0f));

    Json pressed = Json::object();
    pressed.set("action", "jump");
    pressed.set("strength", 0.75);
    pressed.set("device", 0);
    REQUIRE_FALSE(fix.bus().trigger(key, Name("action.pressed"), pressed, 0).error.has_value());
    CHECK(state.strength(Name("jump")) == doctest::Approx(0.75f));

    Json released = Json::object();
    released.set("action", "jump");
    released.set("device", 0);
    REQUIRE_FALSE(fix.bus().trigger(key, Name("action.released"), released, 0).error.has_value());
    CHECK(state.strength(Name("jump")) == doctest::Approx(0.0f));

    // An unrelated event on the same channel is silently ignored, not a
    // crash (ActionState filters by event name before touching the payload).
    REQUIRE_FALSE(
        fix.bus().trigger(key, Name("some.other_event"), Json::object(), 0).error.has_value());
}

TEST_CASE("input.action_state: get_vector(StickDesc) composes four live action strengths") {
    midday::testkit::SimFixture fix;
    ActionState state;
    const EventKey key = EventKey::named(Name("global"));
    REQUIRE_FALSE(fix.bus().subscribe(state, key).has_value());

    StickDesc stick;
    stick.up = "move_up";
    stick.down = "move_down";
    stick.left = "move_left";
    stick.right = "move_right";
    stick.deadzone = 0.2f;

    auto press = [&](std::string_view action) {
        Json payload = Json::object();
        payload.set("action", action);
        payload.set("strength", 1.0);
        payload.set("device", 0);
        REQUIRE_FALSE(fix.bus().trigger(key, Name("action.pressed"), payload, 0).error.has_value());
    };
    // up + right pressed, down/left never pressed: the SAME diagonal as the
    // pure-primitive fixture above (up=pos_y, right=pos_x, left/down=0).
    press("move_up");
    press("move_right");

    const Vec2 vector = state.get_vector(stick);
    CHECK(vector.x == doctest::Approx(0.70710678f));
    CHECK(vector.y == doctest::Approx(0.70710678f));
}
