# core/input

Named action maps, the runtime action-state cache, the virtual-stick/touch
`get_vector` query, and the public/stable/permanent synthetic injection API
(m1-input-actions, spec §228-229 / §508-509 / §707). The `*.input.yaml` /
`*.input_profile.yaml` loaders and `ActionMapDecl` data model live in
`core/loader` (one loader, ever — see `core/loader/loader.h`'s "input action
maps" section and `formats/loader_yaml.md`); this library is the RUNTIME
half that sits on top of them.

- `action_state.h` / `action_state.cpp` — `combine_vector()`: the Godot
  `InputMap::get_vector` formula verbatim (circular deadzone + inverse-lerp
  remap, bit-portable float math), which unifies analog sticks AND digital
  WASD composites without a separate "normalize" flag. `ActionState`: an
  `EventListener` that caches every action's current strength off
  `action.pressed`/`action.released` on the `global` channel, plus
  `get_vector(neg_x, pos_x, neg_y, pos_y, deadzone)` and a `StickDesc`
  convenience overload tying the query straight to an authored composite.
- `inject.h` / `inject.cpp` — `ActionInjector`: THE synthetic injection API
  the testkit is built on. Resolves a `RawInput` (device + control + edge)
  against a loaded `ActionMapDecl` and queues `action.pressed`/
  `action.released` onto the tick loop's EXISTING `inject_input` seat
  (`core/tick/tick_loop.h`, phase 2) — no second injection path, and no real
  OS/device backend lives here (that's `m7-platform`). `inject_at(tick, raw)`
  is the foolproof testkit variant: refuses loudly
  (`input.tick_mismatch`) unless the injection lands on EXACTLY the
  requested tick, instead of silently drifting one tick off.
- `action_state_test.cpp` — the `input.vector` numeric fixture (hand-computed
  expected values against the Godot formula) plus `input.action_state`'s
  bus-driven strength/vector integration tests.
- `inject_test.cpp` — `input.inject`: a synthetic input at tick 42 triggers
  `action.pressed` at tick 42, journaled as a root record (the flagship exit
  test); tick-mismatch refusal, unbound-control no-op, and the
  `action.released` payload shape.

Decisions: the action-map file convention, the injection API surface, the
`get_vector` semantics, the conflict definition, and the overlay shape are
this node's calls (SPEC-GAP: the spec fixes no file convention here, exactly
like `*.events.yaml` before it) — see `core/loader/loader.h`'s header
comment and `formats/loader_yaml.md` for the reasoning. Next consumers:
`m1-scene-format`/game code wiring an action map into a running scene (NOT
this node's job — scope stays DATA + the seam), `m7-platform` real device
backends (which will construct `RawInput` from actual hardware instead of a
test harness).
