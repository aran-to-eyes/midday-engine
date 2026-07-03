# core/base

Primitives every other subsystem stands on (m0-core-primitives):

- `json.h` / `json_write.cpp` / `json_parse.cpp` — THE tree-wide JSON: insertion-ordered
  objects, deterministic shortest-round-trip serialization, strict RFC 8259 parse
  (duplicate keys, bad UTF-8, and unescaped controls rejected; depth capped) with
  structured `origin:line:col` errors, never exceptions. The CLI's former write-only
  `cli/json.*` migrated here (D-BUILD-003).
- `error.h` — the single structured Error envelope (`code`/`message`/`details`) that
  `cli/envelope.h` wraps and journals will reuse; lossless JSON round trip.
- `name.h` — interned `Name`: ids are XXH3-64 content hashes — identical across runs,
  platforms, and intern order (never sequential handout); id 0 reserved for empty;
  detected 64-bit collisions abort.
- `arena.h` — frame/bump allocator: offset-aligned (layout is a pure function of the
  allocation sequence), block-retaining reset, no destructors by contract. Consumers:
  tick-loop frame packets (m0-tick-loop), loader scratch (m0-yaml-loader-run).
- `log.h` — machine-readable logging: one JSONL line per record conforming to
  `formats/log_record.schema.json`; `ts` is a monotonic counter or sim tick, never
  wall clock; sinks = stderr + capture-for-tests. The only sanctioned logging path.

Deliberately absent (no concrete consumer yet — add with the first one):
small-vector (`std::vector` suffices for every current M0 consumer; revisit at
m0-ecs-core), `Result<T>` (parse uses `JsonParseResult`; a general expected-type
waits for the loader), hash maps/sets (std versions fine while iteration order
never reaches output).

Tests: `core.*` doctest cases in `*_test.cpp` beside the code, compiled into the
`midday` binary (`midday selftest --filter 'core.*'`).
