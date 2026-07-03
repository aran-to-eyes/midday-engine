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
