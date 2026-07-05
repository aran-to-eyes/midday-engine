// core/journal/writer.h — the run.mrj bundle writer: the engine's flight
// recorder (spec section 12). Append-only, streaming, bounded memory; FLIGHT
// recording is on by CONSTRUCTION — creating a Writer means recording
// (Zenith D026: the first `midday run` records without opt-in).
//
// Determinism contract (spec section 4.3, D-BUILD-031): given an identical
// record submission sequence, an identical config, and an identical explicit
// flush() sequence, the bundle bytes are identical on every platform —
// journals are THE byte-compare artifact of the determinism lanes. Pinned
// zstd (vendored 1.5.7, fixed level, single-threaded, no wall clock anywhere
// in any frame) makes that hold; the dual-write selftest proves it.
//
// Id policy (D-BUILD-032): EVERY submission consumes the next monotonic id,
// including submissions filtered out by the tier config — so the FLIGHT
// stream's bytes are invariant under snapshot/trace enablement (recording
// more must never perturb the causality skeleton). record() returns the
// consumed id either way; 0 is returned only when the writer refused the
// record (sticky error).
//
// Errors are sticky ostream-style: the first failure poisons the writer,
// record() starts returning 0, and status()/flush()/close() surface the
// structured Error. Nothing throws.

#pragma once

#include "core/base/error.h"
#include "core/base/json.h"
#include "core/journal/bundle.h"
#include "core/journal/record.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace midday::journal {

struct WriterConfig {
    // identity (covered by the replay-identity hash)
    std::string engine_version;                       // e.g. "0.1.0"
    std::string api_compat_hash = "0000000000000000"; // slot until m0-api-json
    std::uint64_t seed = 0;
    TierConfig tiers;                                            // FLIGHT is not a knob: always on
    std::uint32_t index_stride_ticks = kDefaultIndexStrideTicks; // >= 1
    // info (never hashed, never byte-compared)
    std::string platform;   // empty -> base::platform_triple()
    std::string created_at; // optional wall clock; empty -> omitted from header.json
};

struct WriterOpenResult; // {writer, error} — defined after Writer

class Writer {
public:
    // Creates <bundle_dir>/ (refusing to touch an existing path:
    // "journal.bundle_exists"), writes header.json immediately, creates the
    // snapshots/ slot directory, and opens the compressed record stream.
    static WriterOpenResult create(std::string_view bundle_dir, const WriterConfig& config);

    Writer(Writer&&) noexcept;
    Writer& operator=(Writer&&) noexcept;
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    ~Writer(); // best-effort close()

    // Append one record. Ticks must be non-decreasing across ALL submissions
    // ("journal.tick_order"); cause_id must be 0 or an already-consumed id
    // ("journal.cause_unknown"). Returns the consumed id (>= 1), or 0 if the
    // writer refused (see status()). Filtered tiers consume their id but
    // write nothing.
    std::uint64_t record(std::uint64_t tick,
                         Tier tier,
                         std::string_view kind,
                         std::uint64_t cause_id,
                         base::Json payload);

    // The sticky error, if any.
    [[nodiscard]] const std::optional<base::Error>& status() const;

    [[nodiscard]] const Header& header() const;

    // Make everything submitted so far decompressible on disk (crash
    // durability). NOTE: flushing inserts a zstd block boundary, so the
    // compressed bytes depend on the flush sequence — call at deterministic
    // points (e.g. tick boundaries) or not at all. Record CONTENT is
    // unaffected.
    std::optional<base::Error> flush();

    // Finalize: end the open zstd frame, write index.json, close the stream.
    // Idempotent. Returns the sticky error if the journal is poisoned (a
    // poisoned bundle gets no index — it must not look valid).
    std::optional<base::Error> close();

private:
    struct State;
    explicit Writer(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

struct WriterOpenResult {
    std::optional<Writer> writer;
    std::optional<base::Error> error;
};

} // namespace midday::journal
