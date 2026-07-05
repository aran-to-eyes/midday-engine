// core/input/inject.h — the SYNTHETIC INPUT INJECTION API (m1-input-actions):
// public, stable, PERMANENT — spec section 4.3 "synthetic injection is a
// public, stable API (the testkit is built on it)". Resolves a raw device
// event against a loaded action map and rides the tick loop's EXISTING
// injection seat (core/tick/tick_loop.h inject_input, the phase-2 FIFO drain)
// straight onto the bus — there is no second path, and no device backend
// lives here (real OS/device input is m7-platform territory; this is the
// DATA-to-bus seam every backend AND every test will eventually share).

#pragma once

#include "core/base/error.h"
#include "core/loader/loader.h"
#include "core/tick/tick_loop.h"

#include <cstdint>
#include <optional>
#include <string>

namespace midday::input {

enum class RawEdge : std::uint8_t {
    kPressed = 0,
    kReleased = 1,
};

// One raw device event, device-and-control identified exactly like a
// BindingDesc (core/loader/loader.h) — the injector's whole job is looking
// up which action(s), if any, that pair is bound to.
struct RawInput {
    loader::DeviceKind device = loader::DeviceKind::kKeyboard;
    std::string control; // must match a BindingDesc::control verbatim
    RawEdge edge = RawEdge::kPressed;
    float strength = 1.0f;         // meaningful only when edge == kPressed (analog bindings)
    std::int64_t device_index = 0; // the action.pressed/released "device" payload field
};

class ActionInjector {
public:
    ActionInjector(tick::TickLoop& loop, const loader::ActionMapDecl& map)
        : loop_(&loop), map_(&map) {}

    // Queues action.pressed/action.released for every action `raw` is bound
    // to (usually exactly one — a validated map has no conflicts) onto the
    // loop's NEXT input phase (inject_input's own FIFO contract). Zero
    // matching bindings is not an error: an unmapped raw control is a
    // legitimate no-op, exactly like an unbound key on a real keyboard.
    std::optional<base::Error> inject(const RawInput& raw);

    // The foolproof testkit variant (the seam's whole point): refuses
    // loudly ("input.tick_mismatch") unless `target_tick` is EXACTLY the
    // tick this injection will land on (current_tick() + 1) — a miscounted
    // tick fails the test that used it, instead of silently drifting one
    // tick off and passing anyway.
    std::optional<base::Error> inject_at(std::uint64_t target_tick, const RawInput& raw);

private:
    tick::TickLoop* loop_;
    const loader::ActionMapDecl* map_;
};

} // namespace midday::input
