# core/base

Primitives every other subsystem stands on (m0-core-primitives):

- `json.h` / `json_write.cpp` / `json_parse.cpp` — THE tree-wide JSON: insertion-ordered
  objects, deterministic shortest-round-trip serialization, strict RFC 8259 parse
  (duplicate keys, bad UTF-8, and unescaped controls rejected; depth capped) with
  structured `origin:line:col` errors, never exceptions. The CLI's former write-only
  `cli/json.*` migrated here (D-BUILD-003).
  **Cross-platform byte guarantee (D-BUILD-015):** double emission goes through
  vendored dragonbox (+ a shorter-of-fixed/scientific formatter, ties to fixed,
  exact expansion for integer forms) and double parsing through vendored
  fast_float — never the standard library's FP conversions, which are absent or
  divergent on some toolchains. Same double, same JSON bytes, on every
  platform/toolchain, permanently; the golden/drift lanes depend on this. The
  guarantee is pinned by the `core.json` known-answer corpus (validated against
  `std::to_chars` over a 64.6M-double cross-check at introduction — any change
  to the conversion stack must show up there as a diff). Integers use
  std::to_chars/from_chars (universal, locale-free).
- `error.h` — the single structured Error envelope (`code`/`message`/`details`) that
  `cli/envelope.h` wraps and journals will reuse; lossless JSON round trip.
- `name.h` — interned `Name`: ids are XXH3-64 content hashes — identical across runs,
  platforms, and intern order (never sequential handout); id 0 reserved for empty;
  detected 64-bit collisions abort.
- `arena.h` — frame/bump allocator: offset-aligned (layout is a pure function of the
  allocation sequence), block-retaining reset, no destructors by contract. Consumers:
  tick-loop frame packets (m0-tick-loop), loader scratch (m0-yaml-loader-run).
- `hex.h` — the canonical hash spelling: 16-digit lowercase hex for 64-bit values,
  used wherever a hash reaches JSON output or CI byte-compares (selftest's
  math_fixture_hash, reflection compat hashes).
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
