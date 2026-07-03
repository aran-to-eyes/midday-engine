// journal.reader.* — 10k-record exact round trip (cause chain included),
// index-driven tick seeking across zstd frames, header validation, and the
// structured replay refusal (the exit-test case).

#include "core/journal/reader.h"
#include "core/journal/test_support.h"
#include "core/journal/writer.h"
#include "doctest/doctest.h"

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace midday::journal;
using midday::journal::test::pinned_config;
using midday::journal::test::slurp;
using midday::journal::test::spew;
using midday::journal::test::TempDir;
using midday::journal::test::unwrap;
namespace base = midday::base;

namespace {

// Deterministic 10k-record script: ticks advance every third record, kinds
// cycle through all tiers, causes chain to the previous record of the tick.
std::vector<Record> write_corpus(const std::string& bundle, std::size_t count) {
    WriterConfig config = pinned_config();
    config.tiers.snapshot = true;
    config.tiers.trace = true;
    auto opened = Writer::create(bundle, config);
    Writer& writer = unwrap(opened.writer);

    static constexpr Tier kTiers[] = {Tier::Flight, Tier::Flight, Tier::Snapshot, Tier::Trace};
    static constexpr std::string_view kKinds[] = {
        "input.key", "bus.trigger", "snapshot.mark", "known_event"};
    std::vector<Record> expected;
    expected.reserve(count);
    std::uint64_t tick_first_id = 0; // first record id of the current tick
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t tick = i / 3;
        if (i % 3 == 0)
            tick_first_id = 0; // new tick: first record roots at external
        base::Json payload = base::Json::object();
        payload.set("n", static_cast<std::int64_t>(i));
        const Tier tier = kTiers[i % 4];
        const std::uint64_t cause = tick_first_id;
        const std::uint64_t id = writer.record(tick, tier, kKinds[i % 4], cause, payload);
        REQUIRE(id == i + 1);
        if (tick_first_id == 0)
            tick_first_id = id;

        Record record;
        record.tick = tick;
        record.tier = tier;
        record.kind = std::string(kKinds[i % 4]);
        record.cause_id = cause;
        record.id = id;
        record.payload = payload;
        expected.push_back(std::move(record));
    }
    REQUIRE_FALSE(writer.close().has_value());
    return expected;
}

} // namespace

TEST_CASE("journal.reader.round_trip_10k_records") {
    TempDir tmp("roundtrip");
    const std::string bundle = tmp.bundle("corpus");
    const std::vector<Record> expected = write_corpus(bundle, 10000);

    auto opened = Reader::open(bundle);
    Reader& reader = unwrap(opened.reader);
    CHECK(reader.index().records == 10000);
    CHECK(reader.index().entries.size() > 1); // ticks reach 3333: multiple frames

    std::vector<bool> seen(expected.size() + 1, false);
    std::size_t at = 0;
    while (true) {
        auto next = reader.next();
        REQUIRE_FALSE(next.error.has_value());
        if (!next.record.has_value())
            break;
        REQUIRE(at < expected.size());
        const Record& record = *next.record;
        // Exact round trip: canonical line bytes equal, field for field.
        CHECK(to_jsonl(record) == to_jsonl(expected[at]));
        // Cause chain intact: 0 (external) or an earlier record of this run.
        CHECK((record.cause_id == 0 || (record.cause_id < record.id && seen[record.cause_id])));
        seen[record.id] = true;
        ++at;
    }
    CHECK(at == expected.size());

    // Index entries honor the stride granularity and stay strictly ordered.
    const Index& index = reader.index();
    for (std::size_t i = 1; i < index.entries.size(); ++i)
        CHECK(index.entries[i].tick >= index.entries[i - 1].tick + index.stride_ticks);
}

TEST_CASE("journal.reader.seek_by_tick") {
    TempDir tmp("seek");
    const std::string bundle = tmp.bundle("corpus");
    const std::vector<Record> expected = write_corpus(bundle, 3000); // ticks 0..999

    auto opened = Reader::open(bundle);
    Reader& reader = unwrap(opened.reader);

    const auto first_id_at_or_after = [&](std::uint64_t tick) -> std::uint64_t {
        for (const Record& record : expected)
            if (record.tick >= tick)
                return record.id;
        return 0;
    };

    for (const std::uint64_t target : {std::uint64_t{0},
                                       std::uint64_t{1},
                                       std::uint64_t{255},
                                       std::uint64_t{256},
                                       std::uint64_t{257},
                                       std::uint64_t{512},
                                       std::uint64_t{999}}) {
        REQUIRE_FALSE(reader.seek_to_tick(target).has_value());
        auto next = reader.next();
        const Record& record = unwrap(next.record);
        CHECK(record.tick >= target);
        CHECK(record.id == first_id_at_or_after(target));
    }

    // Seeking past the end reads cleanly to end-of-journal.
    REQUIRE_FALSE(reader.seek_to_tick(100000).has_value());
    auto next = reader.next();
    CHECK_FALSE(next.record.has_value());
    CHECK_FALSE(next.error.has_value());
}

TEST_CASE("journal.reader.replay_refusal_is_structured") {
    TempDir tmp("refusal");
    const std::string bundle = tmp.bundle("run");
    write_corpus(bundle, 30);

    SUBCASE("engine version drift refuses") {
        Expectations expect;
        expect.engine_version = "9.9.9-other";
        auto opened = Reader::open(bundle, expect);
        CHECK_FALSE(opened.reader.has_value());
        const base::Error& error = unwrap(opened.error);
        CHECK(error.code == "journal.replay_refusal");
        CHECK(error.details.find("field")->as_string() == "engine_version");
        CHECK(error.details.find("expected")->as_string() == "9.9.9-other");
        CHECK(error.details.find("found")->as_string() == "0.0.0-fixture");
    }

    SUBCASE("api compat hash drift refuses") {
        Expectations expect;
        expect.api_compat_hash = "ffffffffffffffff";
        auto opened = Reader::open(bundle, expect);
        const base::Error& error = unwrap(opened.error);
        CHECK(error.code == "journal.replay_refusal");
        CHECK(error.details.find("field")->as_string() == "api_compat_hash");
    }

    SUBCASE("replay identity pin refuses") {
        Expectations expect;
        expect.replay_identity = "0123456789abcdef";
        auto opened = Reader::open(bundle, expect);
        const base::Error& error = unwrap(opened.error);
        CHECK(error.code == "journal.replay_refusal");
        CHECK(error.details.find("field")->as_string() == "replay_identity");
    }

    SUBCASE("matching expectations open cleanly") {
        auto probe = Reader::open(bundle);
        Expectations expect;
        expect.engine_version = "0.0.0-fixture";
        expect.api_compat_hash = "0000000000000000";
        expect.replay_identity = unwrap(probe.reader).header().replay_identity();
        auto opened = Reader::open(bundle, expect);
        CHECK(opened.reader.has_value());
    }
}

TEST_CASE("journal.reader.validation_failures_are_structured") {
    TempDir tmp("corrupt");

    SUBCASE("missing bundle") {
        auto opened = Reader::open((tmp.path / "nowhere.mrj").string());
        CHECK(unwrap(opened.error).code == "journal.io");
    }

    SUBCASE("tampered identity fails the integrity hash") {
        const std::string bundle = tmp.bundle("tampered");
        write_corpus(bundle, 10);
        const fs::path header_path = fs::path(bundle) / kHeaderFile;
        std::string bytes = slurp(header_path);
        const std::string needle = "\"seed\":7";
        const std::size_t where = bytes.find(needle);
        REQUIRE(where != std::string::npos);
        bytes.replace(where, needle.size(), "\"seed\":8");
        REQUIRE(spew(header_path, bytes));

        auto opened = Reader::open(bundle);
        CHECK(unwrap(opened.error).code == "journal.header_corrupt");
    }

    SUBCASE("empty journal round-trips") {
        const std::string bundle = tmp.bundle("empty");
        {
            auto opened = Writer::create(bundle, pinned_config());
            REQUIRE_FALSE(unwrap(opened.writer).close().has_value());
        }
        auto opened = Reader::open(bundle);
        Reader& reader = unwrap(opened.reader);
        CHECK(reader.index().records == 0);
        CHECK(reader.index().entries.empty());
        auto next = reader.next();
        CHECK_FALSE(next.record.has_value());
        CHECK_FALSE(next.error.has_value());
    }
}
