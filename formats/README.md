# formats

Scene/model/material text format schemas and validators.

- `cli_envelope.schema.json` — the single definition of the CLI JSON envelope (C++ counterpart: `cli/envelope.h`).
- `log_record.schema.json` — the JSONL log record emitted by `core/base/log.*` (mirrored by the `core.log.*` selftests; change both together).
