# ts/codegen — THE code generator (self-hosted, m0-codegen-selfhost)

The REAL engine code generator: TypeScript running on the embedded QuickJS
(built through the toolchain's content-hash cache — no Node anywhere). It
consumes `engine_api.json` text and emits the four committed `api/`
artifacts — `engine.d.ts`, `schema_manifest.json`, `api_docs.md`,
`bindings_spec.json` — **byte-identically** to the TEMPORARY native
bootstrap (`tools/codegen_bootstrap`). It is **authoritative** since
`m0-codegen-selfhost`; the bootstrap tool remains only as the
byte-equivalence pin until it retires post-M0. No subsystem ever gets
hand-written bindings.

```
midday api codegen [<engine_api.json>] [--out-dir api] [--cache-dir <dir>]
midday api codegen --bootstrap ...          # the native tool's library path
midday api codegen --verify-equivalence ... # run BOTH, byte-compare all four
```

- Byte contract (every formatting rule, incl. the number rules JS must
  reproduce): `api/CODEGEN.md`.
- Modules: `generator.ts` (entry: `__midday_codegen_run`), `json.ts`
  (strict parser keeping canonical int64 tokens + the core-writer-exact
  serializer), `model.ts` (validation walk + shared text rules), one
  emitter per artifact (`dts.ts`, `manifest.ts`, `docs.ts`, `bindings.ts`).
- Host surface (`host.d.ts`, wired by `selfhost.cpp` through ScriptRuntime,
  SIM profile — the generator provably cannot read the clock):
  `readInput()`, `writeOutput(name, content)`, `log(...)`. The generator is
  a TOOL: it typechecks against `host.d.ts`, never `api/engine.d.ts`.
- Exit classes (`midday api codegen`): 3 = your document (json.parse,
  api.malformed, codegen.unknown_type/malformed/duplicate_symbol);
  1 = the generator or toolchain (codegen.selfcheck, codegen.internal,
  codegen.io.write, script.*); 2 = usage.
- Tests: `midday selftest --filter 'codegen.*'` — the `codegen.selfhost.*`
  cases byte-compare both generators over the synthetic golden corpus, the
  number-edge corpus (`testkit/codegen_corpus.h`), the live document, and
  its CLI envelope form; verify.sh and the CI drift lane regenerate the
  committed artifacts via THIS generator and keep `--verify-equivalence`
  as a standing gate.
