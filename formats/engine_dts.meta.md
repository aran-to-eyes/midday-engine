# engine.d.ts — validation conventions (pre-tsc)

`api/engine.d.ts` is generated from `api/engine_api.json` by the codegen
chain (bootstrap: `tools/codegen_bootstrap`; authoritative from
`m0-codegen-selfhost` on). A `.d.ts` has no JSON meta-schema, so until the
TypeScript toolchain exists in-tree its validity is checked **structurally**
(`codegen::dts_shape_errors`, run by the `codegen.*` selftests AND by the
tool itself after every generation):

1. Balanced `{`/`}` on non-comment lines (comment lines — leading `//` or
   `/*` — are skipped; generated doc text lives only in comments), never
   dipping negative.
2. Completeness against the source document: every event contributes both
   its payload interface (`interface <Pascal>Event`) and its quoted
   name key in `EventPayloads`; every expression function appears as
   `function <name>(`; every verb appears as its `<Pascal>VerbArgs`
   interface and its quoted key in `VerbArgsByName`; every class appears as
   its interface and its quoted key in `Classes`.
3. No unresolved-generation tokens on non-comment lines: `TODO`, `FIXME`,
   `XXX`, `PLACEHOLDER`.

Formatting rules and the TypeDesc → TypeScript mapping table:
`api/CODEGEN.md` (the byte contract the self-hosted generator must match).

**tsc-level validation** (the declarations actually compile under
`tsc --strict`): in place since `m0-quickjs-ts-toolchain` — the TS toolchain
compiles every program against `api/engine.d.ts` with `skipLibCheck` OFF, so
`midday script check <any .ts>` (and the `script.toolchain` selftests, which
verify.sh runs) tsc-validates the generated declarations on every check.
This structural check remains as the fast, toolchain-free tier.
