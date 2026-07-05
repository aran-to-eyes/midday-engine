# ts/toolchain

The TypeScript toolchain: the vendored compiler (`third_party/typescript/
typescript.js`, data — never compiled or linked) running ON the embedded
QuickJS. No Node anywhere (Aurora D-11).

- `Toolchain::check` = typecheck against `api/engine.d.ts` (skipLibCheck OFF:
  every check also tsc-validates the generated declarations) + the engine
  lint pack; `build` = check + transpile + content-hash cache; `load_module`
  = build through the cache and evaluate on a `ScriptRuntime` (relative
  imports resolve through the same cache; bare `midday/<name>` specifiers
  are the engine module surface — `ts/lib/<name>.ts`, mirrored by the
  canonical tsc `paths` mapping (D-BUILD-072); every other bare specifier
  refuses).
- **Cache**: XXH3-128 over length-prefixed segments — fingerprint(schema +
  lint pack version, canonical options JSON, typescript.js, driver.js,
  engine.d.ts, every `ts/lib/*.ts` path+bytes) then key(fingerprint,
  source). Content-addressed, path-free,
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
  failures, 2 for usage. `midday script bench` (the batch-binding budget
  harness) lives in ts/runtime and drives fixtures through this toolchain.
- **Component schema extraction** (`Toolchain::extract`, m1-ts-components,
  `midday script extract <path> --out <file>`): runs check() and, ONLY if
  it comes back clean, an AST walk (driver.js, same "never regex over
  text" discipline as the lint pack) over the ENTRY file's top-level
  `@component()`-decorated classes — the code is NEVER run. Field/param/
  return types come from an explicit type annotation, or (fields with no
  annotation) the initializer's literal kind; `@field({...})` args and
  literal `= value` defaults are read as literals only (numeric, string,
  boolean, or an array of those) — nothing is evaluated. An unrecognized
  shape pushes a `"schema"`-kind diagnostic into the SAME array the type/
  lint passes use, so it fails validate-before-write exactly like a type
  error. The CLI writes `{format_version, components}` to a PROJECT-LEVEL
  file (`--out`, required) — never `api/schema_manifest.json`, which stays
  engine-only and codegen-owned (api/CODEGEN.md).
- **`midday` bare specifier vs `midday/<name>`**: `midday/<name>` (with a
  slash) resolves to `ts/lib/<name>.ts` via the tsc `paths` mapping AND the
  runtime resolver, identically (D-BUILD-072). Bare `midday` (no slash,
  the m1-ts-components component-authoring surface: `Component`,
  `@component`/`@field`, `StateScript`, `events`, `world.query`,
  `Transform`) is deliberately NOT in `paths` — tsc always prefers an
  in-program AMBIENT module declaration over paths-based file resolution
  for an EXACT specifier match (confirmed empirically), so a competing
  `paths` entry for exact `"midday"` would sit inert once `engine.d.ts`
  declares `declare module "midday"`. The type surface is therefore
  ambient (api/CODEGEN.md "Script component API"); `ts/lib/component.ts`
  (re-exported by `ts/lib/index.ts`, the RUNTIME resolver's target for
  bare `midday`) is the real implementation, kept in sync by hand. The
  `"decorators"` `lib` entry (alongside `es2020`) is TC39 stage-3 decorator
  *types* — the vendored compiler is >= 5.0, so `experimentalDecorators`
  is never needed.
