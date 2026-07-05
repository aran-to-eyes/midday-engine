#include "core/journal/reader.h"

#include "core/base/file_io.h"
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

// The journal's stable IO error code, passed to the shared file seam.
constexpr std::string_view kIoCode = "journal.io";

using base::ReadFileResult;
using base::seek_absolute;

base::Error refusal(std::string_view field,
                    std::string_view expected,
                    std::string_view found,
                    const fs::path& bundle) {
    base::Error error;
    error.code = "journal.replay_refusal";
    error.message = "refusing replay: " + std::string(field) +
                    " mismatch (a replay against drifted "
                    "code is a lie)";
    error.details.set("field", field);
    error.details.set("expected", expected);
    error.details.set("found", found);
    error.details.set("bundle", bundle.string());
    return error;
}

std::optional<base::Error>
check_expectations(const Expectations& expect, const Header& header, const fs::path& bundle) {
    if (!expect.engine_version.empty() && expect.engine_version != header.engine_version)
        return refusal("engine_version", expect.engine_version, header.engine_version, bundle);
    if (!expect.api_compat_hash.empty() && expect.api_compat_hash != header.api_compat_hash)
        return refusal("api_compat_hash", expect.api_compat_hash, header.api_compat_hash, bundle);
    if (!expect.replay_identity.empty() && expect.replay_identity != header.replay_identity())
        return refusal("replay_identity", expect.replay_identity, header.replay_identity(), bundle);
    return std::nullopt;
}

} // namespace

struct Reader::State {
    fs::path dir;
    std::string journal_origin; // (dir / kJournalFile).string(), computed at open
    Header header;
    Index index;
    FILE* file = nullptr;
    ZSTD_DCtx* dctx = nullptr;

    std::vector<char> in; // compressed input buffer
    std::size_t in_pos = 0;
    std::size_t in_len = 0;
    bool file_eof = false;
    std::vector<char> chunk; // one decompressed chunk
    std::string buf;         // decoded bytes pending line extraction
    std::size_t buf_pos = 0;
    bool frame_done = true; // no frame mid-decode
    std::uint64_t skip_below = 0;
    std::optional<base::Error> error; // sticky

    State() = default;
    State(const State&) = delete;
    State& operator=(const State&) = delete;
    State(State&&) = delete;
    State& operator=(State&&) = delete;

    ~State() {
        if (dctx != nullptr)
            ZSTD_freeDCtx(dctx);
        if (file != nullptr)
            std::fclose(file);
    }
};

Reader::Reader(std::unique_ptr<State> state) : state_(std::move(state)) {}

Reader::Reader(Reader&&) noexcept = default;
Reader& Reader::operator=(Reader&&) noexcept = default;
Reader::~Reader() = default;

ReaderOpenResult Reader::open(std::string_view bundle_dir, const Expectations& expect) {
    auto state = std::make_unique<State>();
    state->dir = fs::path(std::string(bundle_dir));

    // header.json: parse, integrity-check, compare against expectations.
    ReadFileResult header_file = base::read_file(state->dir / kHeaderFile, kIoCode);
    if (header_file.error)
        return {std::nullopt, std::move(header_file.error)};
    base::Json::ParseResult header_json =
        base::Json::parse(header_file.bytes, (state->dir / kHeaderFile).string());
    if (header_json.error)
        return {std::nullopt, make_error("journal.header_corrupt", header_json.error->to_string())};
    HeaderParseResult header = Header::from_json(header_json.value);
    if (!header.header.has_value())
        return {std::nullopt, std::move(header.error)};
    state->header = std::move(*header.header);
    if (auto refused = check_expectations(expect, state->header, state->dir))
        return {std::nullopt, std::move(refused)};

    // index.json: parse and cross-check against the header.
    ReadFileResult index_file = base::read_file(state->dir / kIndexFile, kIoCode);
    if (index_file.error)
        return {std::nullopt, std::move(index_file.error)};
    base::Json::ParseResult index_json =
        base::Json::parse(index_file.bytes, (state->dir / kIndexFile).string());
    if (index_json.error)
        return {std::nullopt, make_error("journal.index_corrupt", index_json.error->to_string())};
    IndexParseResult index = Index::from_json(index_json.value);
    if (!index.index.has_value())
        return {std::nullopt, std::move(index.error)};
    state->index = std::move(*index.index);
    if (state->index.stride_ticks != state->header.index_stride_ticks)
        return {std::nullopt,
                make_error("journal.index_corrupt", "index stride disagrees with header")};

    const fs::path journal_path = state->dir / kJournalFile;
    state->journal_origin = journal_path.string();
    state->file = base::open_file(journal_path, "rb");
    if (state->file == nullptr)
        return {std::nullopt,
                make_error("journal.io", "cannot open " + journal_path.string() + " for reading")};
    state->dctx = ZSTD_createDCtx();
    if (state->dctx == nullptr)
        return {std::nullopt, make_error("journal.compress", "cannot create zstd context")};
    state->in.resize(ZSTD_DStreamInSize());
    state->chunk.resize(ZSTD_DStreamOutSize());

    return {Reader(std::move(state)), std::nullopt};
}

const Header& Reader::header() const {
    return state_->header;
}

const Index& Reader::index() const {
    return state_->index;
}

Reader::NextResult Reader::next() {
    State& s = *state_;
    if (s.error)
        return {std::nullopt, s.error};

    while (true) {
        // 1. Hand out the next complete line, skipping seek remainders.
        const std::size_t newline = s.buf.find('\n', s.buf_pos);
        if (newline != std::string::npos) {
            const std::string_view line(s.buf.data() + s.buf_pos, newline - s.buf_pos);
            s.buf_pos = newline + 1;
            RecordParseResult parsed = record_from_line(line, s.journal_origin);
            if (!parsed.record.has_value()) {
                s.error = std::move(parsed.error);
                return {std::nullopt, s.error};
            }
            if (parsed.record->tick < s.skip_below)
                continue;
            return {std::move(parsed.record), std::nullopt};
        }
        if (s.buf_pos > 0) {
            s.buf.erase(0, s.buf_pos);
            s.buf_pos = 0;
        }

        // 2. Refill the compressed input buffer.
        if (s.in_pos == s.in_len && !s.file_eof) {
            s.in_len = std::fread(s.in.data(), 1, s.in.size(), s.file);
            s.in_pos = 0;
            if (s.in_len < s.in.size()) {
                if (std::ferror(s.file) != 0) {
                    s.error = make_error("journal.io", "read failed on journal stream");
                    return {std::nullopt, s.error};
                }
                s.file_eof = true;
            }
        }
        if (s.in_pos == s.in_len && s.file_eof) {
            if (!s.frame_done) {
                s.error = make_error("journal.compress", "journal stream ends mid-frame");
                return {std::nullopt, s.error};
            }
            if (!s.buf.empty()) {
                s.error = make_error("journal.record_corrupt", "journal ends mid-line");
                return {std::nullopt, s.error};
            }
            return {std::nullopt, std::nullopt}; // clean end
        }

        // 3. Decompress one chunk (standard frames, possibly concatenated).
        ZSTD_inBuffer input{s.in.data(), s.in_len, s.in_pos};
        ZSTD_outBuffer output{s.chunk.data(), s.chunk.size(), 0};
        const std::size_t ret = ZSTD_decompressStream(s.dctx, &output, &input);
        if (ZSTD_isError(ret) != 0U) {
            s.error = make_error("journal.compress", ZSTD_getErrorName(ret));
            return {std::nullopt, s.error};
        }
        s.in_pos = input.pos;
        s.frame_done = ret == 0;
        if (s.frame_done) // be explicit: the next input byte starts a new frame
            ZSTD_DCtx_reset(s.dctx, ZSTD_reset_session_only);
        s.buf.append(s.chunk.data(), output.pos);
    }
}

std::optional<base::Error> Reader::seek_to_tick(std::uint64_t tick) {
    State& s = *state_;
    if (s.error)
        return s.error;

    // Nearest preceding frame; before the first entry means the stream start.
    std::uint64_t frame_offset = 0;
    for (const IndexEntry& entry : s.index.entries) {
        if (entry.tick > tick)
            break;
        frame_offset = entry.frame_offset;
    }

    if (seek_absolute(s.file, frame_offset) != 0) {
        s.error = make_error("journal.io", "seek failed on journal stream");
        return s.error;
    }
    ZSTD_DCtx_reset(s.dctx, ZSTD_reset_session_only);
    s.in_pos = 0;
    s.in_len = 0;
    s.file_eof = false;
    s.buf.clear();
    s.buf_pos = 0;
    s.frame_done = true;
    s.skip_below = tick;
    return std::nullopt;
}

} // namespace midday::journal
