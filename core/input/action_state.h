// core/input/action_state.h — the POLLING side of the input system
// (m1-input-actions): tracks the current strength of every named action
// (Godot InputMap::action_states precedent, godot/core/input/input.cpp) and
// exposes the virtual-stick/touch abstraction's `get_vector` query (spec
// section 4.3: "action maps abstract touch — virtual sticks, gesture
// bindings"). Subscribes to the SAME action.pressed/action.released events
// the synthetic injector (core/input/inject.h) and any future real device
// backend both drive — one state cache, one source of truth for "what is
// this action doing right now".
//
// combine_vector() is the pure primitive: Godot's circular-deadzone +
// inverse-lerp remap (input.cpp:579 get_vector), bit-portable float math
// (core/math/vec.h policy: +, -, *, /, sqrt only). It unifies analog sticks
// AND digital composites (WASD) without a separate "normalize" flag — four
// 0/1 digital strengths already produce a length > 1 on a diagonal, which
// the SAME renormalization branch handles.

#pragma once

#include "core/base/name.h"
#include "core/bus/bus.h"
#include "core/loader/loader.h"
#include "core/math/vec.h"

#include <cstdint>
#include <unordered_map>

namespace midday::input {

// The Godot InputMap::get_vector formula, verbatim (godot/core/input/
// input.cpp:579-603): circular deadzone, then either the zero vector, the
// unit-length clamp, or an inverse-lerp remap of (deadzone, 1) -> (0, 1).
[[nodiscard]] math::Vec2
combine_vector(float neg_x, float pos_x, float neg_y, float pos_y, float deadzone);

// Tracks action.pressed/action.released on the "global" channel (builtin_
// events.cpp: "Key: global") and answers strength/get_vector queries.
// Digital bindings report strength 1 (spec vocabulary); action.released
// zeros the cached strength. An action never pressed reports strength 0 —
// there is no "unknown action" state, only "not currently active".
class ActionState : public bus::EventListener {
public:
    void on_event(bus::Bus& bus, const bus::EventView& event) override;

    [[nodiscard]] float strength(base::Name action) const;

    // The Godot-precedent composite over four named actions.
    [[nodiscard]] math::Vec2 get_vector(base::Name neg_x,
                                        base::Name pos_x,
                                        base::Name neg_y,
                                        base::Name pos_y,
                                        float deadzone) const;

    // Convenience overload driven straight from a loaded StickDesc (the
    // action-map file format's `sticks:` section, core/loader/loader.h) —
    // ties the runtime query directly to the authored composite.
    [[nodiscard]] math::Vec2 get_vector(const loader::StickDesc& stick) const;

private:
    std::unordered_map<std::uint64_t, float> strengths_; // base::Name::id() -> strength
};

} // namespace midday::input
