#include "core/journal/writer.h"

#include "core/base/platform.h"
#include "zstd.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace midday::journal {

namespace fs = std::filesystem;

namespace {

base::Error make_error(std::string_view code, std::string message) {
    base::Error error;
    error.code = std::string(code);
    error.message = std::move(message);
    return error;
}

std::optional<base::Error> write_file(const fs::path& path, std::string_view bytes) {
    FILE* file = std::fopen(path.string().c_str(), "wb");
    if (file == nullptr)
        return make_error("journal.io", "cannot open " + path.string() + " for writing");
    const bool ok =
        bytes.empty() || std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    const bool closed = std::fclose(file) == 0;
    if (!ok || !closed)
        return make_error("journal.io", "short write to " + path.string());
    return std::nullopt;
}

} // namespace

struct Writer::State {
    fs::path dir;
    Header header;
    FILE* file = nullptr;
    ZSTD_CCtx* cctx = nullptr;
    std::vector<char> out;

    std::uint64_t next_id = 1;
    std::uint64_t written = 0; // records physically in the stream
    std::uint64_t last_tick = 0;
    bool any_submitted = false;
    bool any_written = false;
    std::uint64_t next_index_tick = 0;
    std::uint64_t plain_bytes = 0;      // decompressed stream size so far
    std::uint64_t compressed_bytes = 0; // journal.jsonl.zst size so far
    bool frame_open = false;
    bool closed = false;
    std::vector<IndexEntry> entries;
    std::optional<base::Error> error;

    State() = default;
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    State(State&&) = delete;
    State& operator=(State&&) = delete;

    ~State() {
        if (cctx != nullptr)
            ZSTD_freeCCtx(cctx);
        if (file != nullptr)
            std::fclose(file);
    }

    void fail(std::string_view code, std::string message) {
        if (!error)
            error = make_error(code, std::move(message));
    }

    // Drive ZSTD_compressStream2 until the input is consumed (and, for
    // e_flush/e_end, until the internal buffers are fully drained).
    void pump(const void* data, std::size_t size, ZSTD_EndDirective mode) {
        ZSTD_inBuffer input{data, size, 0};
        while (true) {
            ZSTD_outBuffer output{out.data(), out.size(), 0};
            const std::size_t remaining = ZSTD_compressStream2(cctx, &output, &input, mode);
            if (ZSTD_isError(remaining) != 0U) {
                fail("journal.compress", ZSTD_getErrorName(remaining));
                return;
            }
            if (output.pos > 0) {
                if (std::fwrite(out.data(), 1, output.pos, file) != output.pos) {
                    fail("journal.io", "short write to journal stream");
                    return;
                }
                compressed_bytes += output.pos;
            }
            const bool done = mode == ZSTD_e_continue ? input.pos == input.size : remaining == 0;
            if (done)
                return;
        }
    }

    void end_frame() {
        if (!frame_open)
            return;
        pump(nullptr, 0, ZSTD_e_end);
        frame_open = false;
    }
};

Writer::Writer(std::unique_ptr<State> state) : state_(std::move(state)) {}

Writer::Writer(Writer&&) noexcept = default;
Writer& Writer::operator=(Writer&&) noexcept = default;

Writer::~Writer() {
    if (state_ == nullptr)
        return;
    try {
        close(); // best effort; errors are already sticky in state_
    } catch (...) {
        // close() reports failures via Error values; only allocation failure
        // can land here, and it must not escape a destructor.
        std::fputs("midday: journal writer close failed during destruction\n", stderr);
    }
}

WriterOpenResult Writer::create(std::string_view bundle_dir, const WriterConfig& config) {
    if (config.index_stride_ticks == 0)
        return {std::nullopt, make_error("journal.config", "index_stride_ticks must be >= 1")};

    auto state = std::make_unique<State>();
    state->dir = fs::path(std::string(bundle_dir));

    std::error_code ec;
    if (fs::exists(state->dir, ec))
        return {std::nullopt,
                make_error("journal.bundle_exists",
                           "bundle path already exists: " + state->dir.string())};
    fs::create_directories(state->dir / kSnapshotsDir, ec);
    if (ec)
        return {std::nullopt,
                make_error("journal.io", "cannot create bundle directory: " + ec.message())};

    state->header.engine_version = config.engine_version;
    state->header.api_compat_hash = config.api_compat_hash;
    state->header.seed = config.seed;
    state->header.tiers = config.tiers;
    state->header.index_stride_ticks = config.index_stride_ticks;
    state->header.platform = config.platform.empty() ? base::platform_triple() : config.platform;
    state->header.created_at = config.created_at;

    if (auto error = write_file(state->dir / kHeaderFile, state->header.to_json().dump() + "\n"))
        return {std::nullopt, std::move(error)};

    const fs::path journal_path = state->dir / kJournalFile;
    state->file = std::fopen(journal_path.string().c_str(), "wb");
    if (state->file == nullptr)
        return {std::nullopt,
                make_error("journal.io", "cannot open " + journal_path.string() + " for writing")};

    state->cctx = ZSTD_createCCtx();
    if (state->cctx == nullptr)
        return {std::nullopt, make_error("journal.compress", "cannot create zstd context")};
    // Pinned parameters (D-BUILD-031): fixed level, content checksum on,
    // content-size flag off (streamed, unknown up front). The vendored
    // library has no multithreading compiled in, so there is no worker-
    // dependent output path at all.
    ZSTD_CCtx_setParameter(state->cctx, ZSTD_c_compressionLevel, kZstdLevel);
    ZSTD_CCtx_setParameter(state->cctx, ZSTD_c_checksumFlag, kZstdChecksum ? 1 : 0);
    ZSTD_CCtx_setParameter(state->cctx, ZSTD_c_contentSizeFlag, 0);
    state->out.resize(ZSTD_CStreamOutSize());

    return {Writer(std::move(state)), std::nullopt};
}

std::uint64_t Writer::record(std::uint64_t tick,
                             Tier tier,
                             std::string_view kind,
                             std::uint64_t cause_id,
                             base::Json payload) {
    State& s = *state_;
    if (s.closed) {
        s.fail("journal.closed", "record() after close()");
        return 0;
    }
    if (s.error)
        return 0;
    // Validation is tier-independent so behavior never varies with the tier
    // config (D-BUILD-032).
    if (s.any_submitted && tick < s.last_tick) {
        s.fail("journal.tick_order",
               "tick " + std::to_string(tick) + " after tick " + std::to_string(s.last_tick));
        return 0;
    }
    if (cause_id >= s.next_id) {
        s.fail("journal.cause_unknown",
               "cause_id " + std::to_string(cause_id) + " has not been assigned yet");
        return 0;
    }
    s.any_submitted = true;
    s.last_tick = tick;
    const std::uint64_t id = s.next_id++;
    if (!s.header.tiers.enabled(tier))
        return id; // id consumed, nothing written: FLIGHT bytes stay invariant

    if (!s.any_written || tick >= s.next_index_tick) {
        s.end_frame();
        if (s.error)
            return 0;
        s.entries.push_back(IndexEntry{.tick = tick,
                                       .record_id = id,
                                       .offset = s.plain_bytes,
                                       .frame_offset = s.compressed_bytes});
        s.next_index_tick = tick + s.header.index_stride_ticks;
        s.frame_open = true;
        s.any_written = true;
    }

    Record entry;
    entry.tick = tick;
    entry.tier = tier;
    entry.kind = std::string(kind);
    entry.cause_id = cause_id;
    entry.id = id;
    entry.payload = std::move(payload);
    const std::string line = to_jsonl(entry) + "\n";

    s.pump(line.data(), line.size(), ZSTD_e_continue);
    if (s.error)
        return 0;
    s.plain_bytes += line.size();
    s.written += 1;
    return id;
}

const std::optional<base::Error>& Writer::status() const {
    return state_->error;
}

const Header& Writer::header() const {
    return state_->header;
}

std::optional<base::Error> Writer::flush() {
    State& s = *state_;
    if (s.error || s.closed)
        return s.error;
    if (s.frame_open)
        s.pump(nullptr, 0, ZSTD_e_flush);
    if (!s.error && std::fflush(s.file) != 0)
        s.fail("journal.io", "fflush failed on journal stream");
    return s.error;
}

std::optional<base::Error> Writer::close() {
    State& s = *state_;
    if (s.closed)
        return s.error;
    s.closed = true;
    s.end_frame();

    if (s.file != nullptr) {
        if (std::fclose(s.file) != 0)
            s.fail("journal.io", "fclose failed on journal stream");
        s.file = nullptr;
    }
    if (s.error)
        return s.error; // a poisoned bundle gets no index

    Index index;
    index.stride_ticks = s.header.index_stride_ticks;
    index.records = s.written;
    index.journal_bytes = s.plain_bytes;
    index.entries = s.entries;
    if (auto error = write_file(s.dir / kIndexFile, index.to_json().dump() + "\n"))
        s.error = std::move(error);
    return s.error;
}

} // namespace midday::journal
