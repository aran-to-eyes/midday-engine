// core/journal/bundle.h — the run.mrj bundle model shared by writer and
// reader: file layout constants, the header (identity/info split), and the
// tick -> byte-offset seek index. Format contract: formats/run_mrj.md +
// formats/mrj_header.schema.json (change together).
//
// Identity vs info (D-BUILD-033): header.json splits into
//   * `identity`  — the deterministic subset (engine version, api compat hash,
//     seed, tier config, compression params, index stride). The REPLAY-IDENTITY
//     hash is XXH3-64 over the canonical dump() bytes of exactly this object.
//   * `info`      — informational only (platform triple, optional created_at).
//     NEVER hashed, NEVER byte-compared; created_at is the single sanctioned
//     wall-clock slot in the whole bundle and is empty by default.

#pragma once

#include "core/base/error.h"
#include "core/base/json.h"
#include "core/journal/record.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace midday::journal {

// Bundle layout: <path>.mrj/ is a directory.
inline constexpr std::string_view kHeaderFile = "header.json";
inline constexpr std::string_view kJournalFile = "journal.jsonl.zst";
inline constexpr std::string_view kIndexFile = "index.json";
inline constexpr std::string_view kSnapshotsDir = "snapshots"; // slots; content lands at M2

inline constexpr std::string_view kHeaderSchema = "midday.mrj.header/1";
inline constexpr std::string_view kIndexSchema = "midday.mrj.index/1";

// Pinned zstd parameters (D-BUILD-031): fixed level, content checksum on,
// single-threaded by construction (the vendored library is built without
// ZSTD_MULTITHREAD). Output bytes are a pure function of the record lines,
// the frame-cut schedule, and the explicit flush sequence.
inline constexpr int kZstdLevel = 3;
inline constexpr bool kZstdChecksum = true;

// Index granularity default: one entry (= one zstd frame) per stride of ticks.
inline constexpr std::uint32_t kDefaultIndexStrideTicks = 256;

// Write-time tier filter. FLIGHT is deliberately not a field: it is always on.
struct TierConfig {
    bool snapshot = false;
    bool trace = false;

    [[nodiscard]] bool enabled(Tier tier) const {
        return tier == Tier::Flight || (tier == Tier::Snapshot && snapshot) ||
               (tier == Tier::Trace && trace);
    }
};

struct HeaderParseResult; // {header, error} — defined after Header
struct IndexParseResult;  // {index, error} — defined after Index

struct Header {
    // identity — covered by replay_identity().
    std::string engine_version;
    std::string api_compat_hash; // 16-digit lowercase hex slot (m0-api-json fills it)
    std::uint64_t seed = 0;
    TierConfig tiers;
    std::uint32_t index_stride_ticks = kDefaultIndexStrideTicks;
    // info — excluded from all hashes and byte-compares.
    std::string platform;   // build platform triple, e.g. "linux-x86_64-gcc"
    std::string created_at; // optional wall clock, empty = omitted (D-BUILD-013)

    // The canonical identity subobject (fixed key order; includes the pinned
    // compression parameters and the vendored zstd version).
    [[nodiscard]] base::Json identity_json() const;

    // hex64(XXH3-64(identity_json().dump())) — the replay-identity hash.
    [[nodiscard]] std::string replay_identity() const;

    // The complete header.json document: {schema, identity, replay_identity, info}.
    [[nodiscard]] base::Json to_json() const;

    // Strict parse + integrity check: the stored replay_identity must match
    // the hash recomputed over the parsed identity object, else
    // "journal.header_corrupt".
    static HeaderParseResult from_json(const base::Json& json);
};

struct HeaderParseResult {
    std::optional<Header> header;
    std::optional<base::Error> error;
};

// One seek point: a zstd frame starts at frame_offset (compressed) and its
// decompressed bytes begin at offset; the first record inside it is record_id
// at `tick`. Seeking never decompresses more than one stride of ticks.
struct IndexEntry {
    std::uint64_t tick = 0;
    std::uint64_t record_id = 0;
    std::uint64_t offset = 0;       // byte offset into the DECOMPRESSED stream
    std::uint64_t frame_offset = 0; // byte offset into journal.jsonl.zst
};

struct Index {
    std::uint32_t stride_ticks = kDefaultIndexStrideTicks;
    std::uint64_t records = 0;       // total records written
    std::uint64_t journal_bytes = 0; // total decompressed bytes
    std::vector<IndexEntry> entries; // strictly increasing tick, offsets, ids

    [[nodiscard]] base::Json to_json() const;

    static IndexParseResult from_json(const base::Json& json);
};

struct IndexParseResult {
    std::optional<Index> index;
    std::optional<base::Error> error;
};

} // namespace midday::journal
