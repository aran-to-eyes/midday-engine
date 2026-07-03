# `run.mrj` — the run journal bundle (spec §12)

The engine's flight recorder. Every run-capable verb records a bundle from its
very first tick — FLIGHT recording is on by construction, never an opt-in
(Zenith D026). Journals are THE byte-compare artifact of the determinism CI
lanes: given the same build, config, and inputs, two runs produce
byte-identical bundles.

A bundle is a **directory** (conventionally named `<something>.mrj/`):

```
run.mrj/
├── header.json        # build/content identity + informational metadata
├── journal.jsonl.zst  # the record stream: zstd frames over JSONL
├── index.json         # tick → byte-offset seek index
└── snapshots/         # world-state sidecar slots (content lands at M2)
```

Implementation: `core/journal/` (writer, reader, record model).
Schemas: `formats/mrj_header.schema.json`, `formats/mrj_record.schema.json`.

## header.json  (`midday.mrj.header/1`)

Written at bundle creation, before any record. Two disjoint sections:

- **`identity`** — the deterministic subset: `engine_version`,
  `api_compat_hash` (16-digit lowercase hex; a zero slot until m0-api-json),
  `seed`, `tiers` (`flight` is always `true` — a header claiming otherwise is
  corrupt), `compression` (pinned parameters, see below), and
  `index_stride_ticks`.
- **`replay_identity`** — `hex64(XXH3-64(dump(identity)))`, the hash over the
  canonical serialized bytes of exactly the `identity` object (same scheme as
  the reflection compat hashes, D-BUILD-021). Readers recompute it; mismatch
  is `journal.header_corrupt`.
- **`info`** — informational ONLY, excluded from every hash and byte-compare:
  `platform` (build triple, e.g. `linux-x86_64-gcc`) and optionally
  `created_at` — the single sanctioned wall-clock slot in the whole bundle
  (D-BUILD-013). Absent unless a caller explicitly supplies it; record content
  NEVER contains wall-clock time.

**Replay refusal:** a replay against drifted code is a lie. `Reader::open`
takes `Expectations` (engine version, api compat hash, and/or a full
replay-identity pin); any mismatch returns a structured
`journal.replay_refusal` error with `details.field/expected/found`.

## journal.jsonl.zst — the record stream

Standard zstd frames over a JSONL stream: one JSON object per line, `\n`
framing. Because the frames are standard,

```sh
zstdcat run.mrj/journal.jsonl.zst | grep known_event
```

works on any machine with the zstd CLI — text stays greppable, bulk stays
compressed. (The verify gate runs exactly this against
`testkit/fixtures/journal/greppable.mrj`, plus a byte-compare of that fixture
against a freshly regenerated bundle.)

### Record shape (`formats/mrj_record.schema.json`)

Fixed key order: `tick`, `tier`, `kind`, `cause_id`, `id`[, `payload`]:

```json
{"tick":42,"tier":"flight","kind":"bus.trigger","cause_id":3,"id":9,"payload":{"event":"hit"}}
```

- `tick` — sim tick, non-decreasing over the stream. Never wall clock.
- `tier` — `flight` | `snapshot` | `trace` (semantics below).
- `kind` — stable dotted identifier of what happened (`input.key`,
  `bus.trigger`, `state.enter`, ...).
- `cause_id` — the `id` of the record that caused this one; `0` = external
  root (player input, scheduler). Cause chains reconstruct mechanically:
  `replay explain` walks them backward.
- `id` — per-run monotonic, starting at 1. **Every submission consumes an id,
  including submissions filtered by the tier config** (D-BUILD-032) — so the
  FLIGHT records' bytes are invariant under snapshot/trace enablement:
  recording more can never perturb the causality skeleton. Id gaps in a
  journal therefore mark filtered records.
- `payload` — kind-specific object; omitted entirely when empty.

### Record tiers

| Tier | When | Contents |
|---|---|---|
| `flight` | **always on** — shipped builds, first run, no knob exists | the causality skeleton: inputs, events, transitions, spawns — enough to re-simulate |
| `snapshot` | dev default (config) | + periodic world-state captures for fast seek (content lands at m2-snapshots) |
| `trace` | opt-in (config) | + high-volume per-tick diagnostics (component deltas, property curves) |

Filtering happens at write time; the enabled set is recorded in
`identity.tiers`.

## index.json  (`midday.mrj.index/1`)

Tick → byte-offset seek index. The writer cuts a **new zstd frame** whenever a
written record's tick first reaches `last_entry_tick + index_stride_ticks`
(default 256; recorded in the header identity), and each cut adds one entry:

```json
{"tick":300,"record_id":4,"offset":243,"frame_offset":143}
```

- `offset` — byte offset into the **decompressed** stream (frame boundaries
  coincide with record-line boundaries by construction).
- `frame_offset` — byte offset of the zstd frame inside `journal.jsonl.zst`.

Seeking to tick T: binary-search the last entry with `tick <= T`, fseek to its
`frame_offset`, decompress forward, skip records with `tick < T` — at most one
stride of ticks is ever re-read. Concatenated frames are still one valid zstd
stream, so greppability is untouched.

`records` counts records physically written; `journal_bytes` is the total
decompressed size.

## Determinism (D-BUILD-031)

Bundle bytes are a pure function of `(record submissions, config, explicit
flush sequence)`:

- zstd is **vendored and pinned** (third_party/zstd, single-file 1.5.7) —
  compressed bytes are a property of the vendored code, not the host.
- Pinned parameters, recorded in the header identity: level 3, content
  checksum on (XXH64 of content — itself deterministic), content-size flag
  off (streamed), **no multithreading compiled in** (the amalgamation is
  built without `ZSTD_MULTITHREAD`, removing every worker-dependent output
  path and the thread-library dependency).
- No timestamps anywhere in the stream or frames; `info.created_at` (header
  only, optional) is excluded from all hashes and compares.
- `Writer::flush()` inserts a zstd block boundary (crash durability), so call
  it at deterministic points (tick boundaries) or not at all — it changes
  compressed bytes, never record content.

The `journal.writer.dual_write_byte_identical` selftest writes two bundles
from independent writers and byte-compares all three files; the verify gate
byte-compares a freshly regenerated bundle against the committed fixture on
every run.
