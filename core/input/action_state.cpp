#include "core/input/action_state.h"

#include "core/base/json.h"

#include <string_view>

namespace midday::input {

math::Vec2 combine_vector(float neg_x, float pos_x, float neg_y, float pos_y, float deadzone) {
    const math::Vec2 raw{pos_x - neg_x, pos_y - neg_y};
    const float length = raw.length();
    if (length <= deadzone)
        return math::Vec2{};
    if (length > 1.0f)
        return raw / length;
    // Math::inverse_lerp(deadzone, 1, length) / length, applied as a scale
    // factor (Godot input.cpp:601): remaps (deadzone, 1) -> (0, 1) along the
    // raw vector's own direction.
    const float scale = (length - deadzone) / (1.0f - deadzone) / length;
    return raw * scale;
}

void ActionState::on_event(bus::Bus& /*bus*/, const bus::EventView& event) {
    const std::string_view name = event.event.view();
    if (name != "action.pressed" && name != "action.released")
        return; // this cache only tracks the two vocabulary action events
    if (!event.payload.is_object())
        return;
    const base::Json* action_field = event.payload.find("action");
    if (action_field == nullptr || !action_field->is_string())
        return;
    const base::Name action(action_field->as_string());
    if (name == "action.released") {
        strengths_[action.id()] = 0.0f;
        return;
    }
    const base::Json* strength_field = event.payload.find("strength");
    strengths_[action.id()] = strength_field != nullptr && strength_field->is_number()
                                  ? static_cast<float>(strength_field->as_double())
                                  : 1.0f; // digital bindings report strength 1 (builtin_events.cpp)
}

float ActionState::strength(base::Name action) const {
    const auto found = strengths_.find(action.id());
    return found == strengths_.end() ? 0.0f : found->second;
}

math::Vec2 ActionState::get_vector(
    base::Name neg_x, base::Name pos_x, base::Name neg_y, base::Name pos_y, float deadzone) const {
    return combine_vector(
        strength(neg_x), strength(pos_x), strength(neg_y), strength(pos_y), deadzone);
}

math::Vec2 ActionState::get_vector(const loader::StickDesc& stick) const {
    // y convention: "up" is POSITIVE y (standard math axes for ground-plane
    // movement — this is a 3D engine's forward/strafe vector, not a 2D
    // screen coordinate, so we deliberately invert Godot's screen-space
    // "up is negative y" 2D convention here).
    return get_vector(base::Name(stick.left),
                      base::Name(stick.right),
                      base::Name(stick.down),
                      base::Name(stick.up),
                      stick.deadzone);
}

} // namespace midday::input
