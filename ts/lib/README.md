# ts/lib

Engine-side TS library shipped to game scripts — the `midday/*` module
surface (D-BUILD-072): a bare specifier `midday/<name>` resolves to
`ts/lib/<name>.ts`, identically at typecheck (canonical tsc `paths` mapping)
and at runtime (`Toolchain::load_module`). Every file here is part of the
toolchain cache fingerprint: editing the library invalidates dependent
script builds soundly.

- `batch.ts` — `midday/batch`: the batch-first binding surface (spec §7,
  m0-batch-bindings). `request({components})` returns a live envelope of
  typed-array views the engine refreshes per phase; `onTick(fn)` installs
  the per-tick entry. Refuses any envelope version but 1.
- `math.ts` — `midday/math`: pooled `Vec3`/`Quat` + `Pool<T>` so
  steady-state ticks allocate ZERO GC bytes (bindings.* fixtures pin it).
  IEEE-exact ops only (+ - * / sqrt); transcendentals arrive through the
  engine bindings, never here.
- `component.ts` — `midday/component`, re-exported by `index.ts` as the
  bare `midday` specifier's RUNTIME backing (m1-ts-components; the type
  surface is ambient in `engine.d.ts` — see `ts/toolchain/README.md` for
  why the two are separate declarations kept in sync by hand). `Component`/
  `StateScript` base classes, `EntityRef` (the concrete
  `{index, generation}` class; `.get/.tryGet/.has/.root` back the ambient
  `midday.EntityRef` interface's same-named methods), `component()`/
  `field()` decorators (metadata shape only — schema extraction never runs
  them, `ts/toolchain/driver.js`), `events.trigger`, `world.query`
  (a per-entity object directory — TS-authored components have no C++
  type, so they can never live in a typed `ecs::Pool<T>`), and the built-in
  `Transform` component. `__attachComponent` is the loader/test seam that
  populates the directory; never a game-facing export (no code-assembled
  entities, `scripts/check_entity_api.py`).
- `index.ts` — the file `ts/toolchain/toolchain.cpp` resolves the bare
  `midday` specifier to (typecheck: N/A, ambient wins; runtime: this file).
  Re-exports `component.ts` in full.
