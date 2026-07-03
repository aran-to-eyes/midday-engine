// core/journal/record.h — the journal record model (spec section 12): the
// engine's flight recorder speaks in Records. One record per journaled effect
// (input, event, transition, spawn, ...), each carrying a parent cause_id so
// causality chains reconstruct mechanically. Serialized as one JSON object per
// line (JSONL) inside journal.jsonl.zst — see formats/run_mrj.md and
// formats/mrj_record.schema.json (change all three together).
//
// Determinism (spec section 4.3): record content NEVER contains wall-clock
// time — `tick` is the sim tick, ids are per-run monotonic counters. Identical
// record sequences serialize to identical bytes on every platform (the core
// JSON writer guarantees this, D-BUILD-015).

#pragma once

#include "core/base/error.h"
#include "core/base/json.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace midday::journal {

// Record tiers (spec section 12). Write-time filtering is configured in the
// bundle header; FLIGHT cannot be disabled — it is the always-on causality
// skeleton (Zenith D026), present from the very first `midday run`.
enum class Tier : std::uint8_t {
    Flight = 0,   // always on: inputs, events, transitions, spawns — enough to re-simulate
    Snapshot = 1, // + periodic world-state captures for fast seek (dev default; content at M2)
    Trace = 2,    // + high-volume per-tick diagnostics (off by default, opt-in)
};

// The schema's `tier` enum strings: "flight" | "snapshot" | "trace".
std::string_view to_string(Tier tier);
std::optional<Tier> tier_from_string(std::string_view text);

struct Record {
    std::uint64_t tick = 0;                    // sim tick — never wall clock
    Tier tier = Tier::Flight;                  //
    std::string kind;                          // stable dotted identifier, e.g. "bus.trigger"
    std::uint64_t cause_id = 0;                // id of the causing record; 0 = external/root
    std::uint64_t id = 0;                      // monotonic per run, first submission = 1
    base::Json payload = base::Json::object(); // kind-specific structured data
};

// One record -> one JSONL line (no trailing newline; the writer owns framing).
// Key order is fixed: tick, tier, kind, cause_id, id[, payload]; an empty
// payload object is omitted entirely.
std::string to_jsonl(const Record& record);

struct RecordParseResult {
    std::optional<Record> record;
    std::optional<base::Error> error;
};

// Strict inverse of to_jsonl: exactly the keys above (payload optional, an
// object when present), kind non-empty, id >= 1, all integers non-negative.
// Anything else -> a structured "journal.record_corrupt" Error.
RecordParseResult record_from_line(std::string_view line, std::string_view origin = "<journal>");

} // namespace midday::journal
