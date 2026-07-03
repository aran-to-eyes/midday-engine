# core/journal

The `run.mrj` flight recorder (m0-journal, spec section 12): the causality
journal every event dispatch, transition, and spawn writes through.

- `record.h` — the record model: `{tick, tier, kind, cause_id, id, payload}`,
  tiers FLIGHT (always-on causality skeleton) / SNAPSHOT (state captures) /
  TRACE (high-volume diagnostics, off by default), fixed JSONL byte shape.
- `bundle.h` — bundle layout, header (identity/info split + replay-identity
  hash), tick -> byte-offset seek index. Format doc: `formats/run_mrj.md`.
- `writer.h` — append-only streaming writer: FLIGHT on by construction
  (recorder creation = recording, Zenith D026), deterministic bytes given
  identical submissions + config (pinned vendored zstd, single-threaded,
  D-BUILD-031), every submission consumes an id even when tier-filtered so
  FLIGHT bytes never vary with the tier config (D-BUILD-032), sticky
  structured errors, bounded memory.
- `reader.h` — validating reader: header integrity + `Expectations` check
  (mismatch = structured `journal.replay_refusal`), streaming iteration,
  index-driven `seek_to_tick`.

The stream stays `zstdcat`-able (standard frames, one JSON object per line);
the verify gate byte-compares a regenerated bundle against
`testkit/fixtures/journal/greppable.mrj` and greps it with the real zstdcat.

Tests: `journal.*` doctest cases beside the code
(`midday selftest --filter 'journal.*'`).
