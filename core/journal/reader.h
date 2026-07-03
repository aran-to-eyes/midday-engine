// core/journal/reader.h — the run.mrj bundle reader: open, validate against
// expectations, stream records, seek by tick through the index. Streaming
// and bounded-memory like the writer; the journal stays zstdcat-able, this
// reader is just the in-engine path.
//
// Replay refusal (spec section 12): a replay against drifted code is a lie.
// open() compares the bundle header against the caller's Expectations and
// returns a structured "journal.replay_refusal" Error on any mismatch —
// details name the field, the expected value, and what the bundle carries.

#pragma once

#include "core/base/error.h"
#include "core/journal/bundle.h"
#include "core/journal/record.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace midday::journal {

// Empty fields accept anything (e.g. a pure inspection tool passes {});
// replay passes the running engine's identity so drift refuses loudly.
struct Expectations {
    std::string engine_version;
    std::string api_compat_hash;
    std::string replay_identity; // full identity-hash pin, strongest check
};

struct ReaderOpenResult; // {reader, error} — defined after Reader

class Reader {
public:
    static ReaderOpenResult open(std::string_view bundle_dir, const Expectations& expect = {});

    Reader(Reader&&) noexcept;
    Reader& operator=(Reader&&) noexcept;
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    ~Reader();

    [[nodiscard]] const Header& header() const;
    [[nodiscard]] const Index& index() const;

    struct NextResult {
        std::optional<Record> record;
        std::optional<base::Error> error; // corrupt line / truncated frame
        // Both empty: clean end of journal.
    };

    // Stream the next record in journal order.
    NextResult next();

    // Reposition so the following next() returns the first record with
    // record.tick >= tick (end-of-journal if none). Seeks to the nearest
    // preceding index frame, so at most one stride of ticks is re-read.
    std::optional<base::Error> seek_to_tick(std::uint64_t tick);

private:
    struct State;
    explicit Reader(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

struct ReaderOpenResult {
    std::optional<Reader> reader;
    std::optional<base::Error> error;
};

} // namespace midday::journal
