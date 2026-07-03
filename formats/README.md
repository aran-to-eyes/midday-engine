# formats

Scene/model/material text format schemas and validators.

- `cli_envelope.schema.json` — the single definition of the CLI JSON envelope (C++ counterpart: `cli/envelope.h`).
- `log_record.schema.json` — the JSONL log record emitted by `core/base/log.*` (mirrored by the `core.log.*` selftests; change both together).
- `run_mrj.md` — the `run.mrj` journal bundle format (spec section 12): directory layout, header identity/info split, record stream, tiers, seek index, determinism guarantees (C++ counterpart: `core/journal/`).
- `mrj_header.schema.json` — the bundle's `header.json` (identity subset is what the replay-identity hash covers).
- `mrj_record.schema.json` — one journal record line inside `journal.jsonl.zst` (pinned by the `journal.record.*` selftests; change together).
