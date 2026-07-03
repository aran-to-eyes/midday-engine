# codegen_bootstrap — TEMPORARY native bootstrap generator

**TEMPORARY, byte-equivalence-covered.** This tool exists only so QuickJS and
the TS toolchain can come up: it is deliberately small, parses
`engine_api.json`, and emits the four generated `api/` artifacts.
`m0-codegen-selfhost` re-implements the generator in TS-on-QuickJS; once the
self-hosted output **byte-matches** this tool's output on the bootstrap
corpus, the self-hosted generator becomes authoritative and this tool is
retired to bootstrap duty only. No subsystem ever gets hand-written bindings.

```
codegen_bootstrap [<engine_api.json>] [--out-dir <dir>]
```

- Input defaults to `api/engine_api.json`; a CLI envelope
  (`midday api dump --json > f`) is unwrapped automatically.
- Outputs to `--out-dir` (default `api`): `engine.d.ts`,
  `schema_manifest.json`, `api_docs.md`, `bindings_spec.json` — byte
  deterministic, drift-gated by verify.sh and the CI drift lane.
- Exit codes: 0 ok · 1 write/self-check failure · 2 usage · 3 invalid input
  (bad JSON, unknown `format_version`, unknown type spelling). Errors are
  structured JSON on stdout.

Every formatting rule (the selfhost byte contract): `api/CODEGEN.md`.
Library: `midday::codegen_bootstrap` (`codegen.h` + one emitter per output);
tests: `midday selftest --filter 'codegen.*'`.
