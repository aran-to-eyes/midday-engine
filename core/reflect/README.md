# core/reflect

The ClassDB-equivalent for an ECS + statechart engine (m0-reflection-events).
Deliberate deviation from Godot: Midday reflects **component classes, events
with typed payload schemas, and free functions** (the expression-language
inventory) — not an Object hierarchy. This registry is THE source for
`engine_api.json` (m0-api-json) and everything generated from it (engine.d.ts,
schema manifest, agent docs). Later nodes ONLY ADD entries: m0-ecs-core adds
component classes, m0-expr-lang adds functions, m0-event-bus consumes the
event vocabulary.

- `type_model.h` — the data-driven type vocabulary: scalar kinds
  (bool/int/float/string/name/vec2-4/quat/color/entity_ref/asset_ref) plus
  composite `array<T>`/`map<T>`. One canonical spelling everywhere
  (`canonical()`/`parse()` round-trip, pinned); `accepts(Json)` decides
  whether a JSON literal inhabits a type — property defaults are validated
  against their declared type at registration (loud abort on mismatch).
- `registry.h` — instance-based `Registry` (no global singleton; the engine
  binds one at boot, tests build their own): `add_class/add_event/
  add_function`, `find_*` by interned Name, deterministic enumeration
  (init-level-major, registration order within a level — never pointer/hash
  order), `to_json()` as the engine_api.json seed. Descriptor addresses are
  stable until their init level tears down. **Compat hashes** (the api-diff
  drift primitive): XXH3-64 over the dump() bytes of a signature-only
  canonical JSON — docs excluded, per-class/per-method/per-event/per-function,
  spelled as 16-digit lowercase hex (`core/base/hex.h`). No exceptions
  anywhere; duplicates and malformed descriptors abort loudly (D-BUILD-011
  ethos). Method/property invocation thunks are NOT here — binding dispatch
  tables arrive with m0-batch-bindings, keyed by the Names registered here.
- `init_levels.h` — `InitLevel` CORE → SERVERS → SCENE → TOOLS and the
  `Lifecycle` driver: per-level hooks run in contribution order, torn down
  exactly mirrored (levels reversed, hooks within a level reversed);
  registrations are stamped with the level being initialized and die with it.
  Visibility IS registry presence: SERVERS symbols do not exist while only
  CORE has initialized (pinned by the reflect.init fixtures).
- `builtin_events.h` — the built-in vocabulary registered at CORE:
  trigger.entered/exited, contact.began/ended, state.finished,
  entity.spawned/despawned, action.pressed/released — each with a typed,
  documented payload schema (D-BUILD-022). `event_payload_type_name()` pins
  the derived codegen type name ("trigger.entered" → `TriggerEntered`).

Tests: `reflect.*` doctest cases beside the code
(`midday selftest --filter 'reflect.*'`), including pinned compat-hash known
answers and the byte-pinned JSON description of trigger.entered.
