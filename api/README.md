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

`engine.d.ts`, `schema_manifest.json`, and docs derive from this document at
m0-codegen-bootstrap.
