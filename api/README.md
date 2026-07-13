# api

`engine_api.json` — generated, canonical, drift-gated. Every reflected class,
event, and expression function plus every CLI verb schema, with per-entry
compat hashes and one top-level `api_compat_hash` (signature-only; docs and
engine_version are outside every hash). Meta-schema:
`formats/engine_api.schema.json`.

- Regenerate: `midday api dump --out api/engine_api.json` (commit the result).
- Drift check: `midday api diff api/engine_api.json` (exit 1 + structured
  added/removed/changed report on any signature change); verify.sh and CI's
  drift lane byte-compare the committed file against a fresh dump.
- Emitter/differ: `api/engine_api.{h,cpp}` (library `midday::api`).

Derived artifacts — generated from `engine_api.json` by the SELF-HOSTED
generator (`ts/codegen`, TS-on-QuickJS, **authoritative** since
`m0-codegen-selfhost`; `midday api codegen`). The TEMPORARY native
bootstrap (`tools/codegen_bootstrap`) byte-matches it and remains only as
the equivalence pin (`midday api codegen --verify-equivalence`) until it
retires post-M0. Committed here and drift-gated the same way (two-run cmp
+ committed-byte cmp + the equivalence gate in verify.sh and CI's drift
lane):

- `engine.d.ts` — agent-facing TypeScript declarations (value types, event
  payload interfaces + name map, expression function signatures, CLI verb
  argument types), plus a trailing ambient `declare module "midday"` block
  (the Script component API's generated event-alias augmentation over
  `ts/lib/component.ts`'s real Component/StateScript/decorator surface —
  m1-ts-components). Structural validation: `formats/engine_dts.meta.md`.
- `schema_manifest.json` — the validate-before-write source (value-type wire
  shapes, event payload schemas, expression signatures; scene/machine formats
  join at m1). Meta-schema: `formats/schema_manifest.schema.json`.
- `api_docs.md` — generated reference: every entry with docs + compat hash.
- `bindings_spec.json` — the glue spec m0-batch-bindings implements
  (call signatures + batch envelope + state-script hook seam + the M2 #12b
  `event_payload_types` bijection driver.js's onEvent extraction consults).

Regenerate all four: `build/dev/midday api codegen` from the repo root.
Every formatting rule (the byte contract both generators obey): `CODEGEN.md`.
