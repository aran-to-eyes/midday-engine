# ts/toolchain

The TypeScript toolchain: the vendored compiler (`third_party/typescript/
typescript.js`, data — never compiled or linked) running ON the embedded
QuickJS. No Node anywhere (Aurora D-11).

- `Toolchain::check` = typecheck against `api/engine.d.ts` (skipLibCheck OFF:
  every check also tsc-validates the generated declarations) + the engine
  lint pack; `build` = check + transpile + content-hash cache; `load_module`
  = build through the cache and evaluate on a `ScriptRuntime` (relative
  imports resolve through the same cache; bare specifiers wait for the
  bindings).
- **Cache**: XXH3-128 over length-prefixed segments — fingerprint(schema +
  lint pack version, canonical options JSON, typescript.js, driver.js,
  engine.d.ts) then key(fingerprint, source). Content-addressed, path-free,
  byte-stable across platforms; only clean builds populate it, so a hit
  soundly skips compile AND check. `midday script build --stats` reports
  `{transpiled, cache_hits}`; the second run is zero re-transpiles.
- **Lint pack (midday-lint/1)**: AST walk in `driver.js` (never regex) —
  no-wall-clock (`Date.now` / `new Date()` / `Date()` / `performance.now`),
  no-unseeded-random (`Math.random`), no-timer (`setTimeout` /
  `setInterval`), each at file:line:col, on every non-declaration source in
  the program, independent of whether the name typechecks. Bypass policy:
  none.
- CLI: `midday script check|build <path>` — exit 3 for type errors AND lint
  hits (structured diagnostics in the payload), 1 for infrastructure
  failures, 2 for usage.
