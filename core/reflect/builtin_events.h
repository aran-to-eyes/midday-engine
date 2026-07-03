// core/reflect/builtin_events.h — the engine's built-in event vocabulary
// (spec section 4.2 + Appendix A), registered at the CORE init level so it
// exists before any server or scene symbol. Every name here reaches
// engine_api.json with its typed payload schema — nothing implicit.
//
// Vocabulary: trigger.entered/exited · contact.began/ended · state.finished ·
// entity.spawned/despawned · action.pressed/released (payload field lists:
// D-BUILD-022; consumers: m0-event-bus dispatch, m0-jolt-minimal contacts,
// m1-input-actions action events, statechart state.finished chaining).

#pragma once

namespace midday::reflect {

class Registry;

// Registers the whole vocabulary. Idempotence is NOT provided — a second
// call is a duplicate registration and aborts (per registry contract).
// The canonical engine boot installs this as its first CORE init hook.
void register_builtin_events(Registry& registry);

} // namespace midday::reflect
