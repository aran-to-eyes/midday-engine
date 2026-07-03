// journal.writer.* + journal.greppability — bundle creation, deterministic
// bytes (dual-write, never a self-diff), tier filtering with id invariance,
// sticky structured errors, and the zstdcat-able stream contract.

#include "core/journal/reader.h"
#include "core/journal/test_support.h"
#include "core/journal/writer.h"
#include "doctest/doctest.h"

#include <cstdlib>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;
using namespace midday::journal;
using midday::journal::test::pinned_config;
using midday::journal::test::slurp;
using midday::journal::test::TempDir;
using midday::journal::test::unwrap;
namespace base = midday::base;

namespace {

// The canonical mixed-tier submission script used by several tests.
void submit_script(Writer& writer, std::vector<std::uint64_t>& flight_ids) {
    base::Json payload = base::Json::object();
    payload.set("button", "jump");
    const std::uint64_t input = writer.record(0, Tier::Flight, "input.key", 0, payload);
    flight_ids.push_back(input);
    writer.record(0, Tier::Trace, "ecs.delta", input, base::Json::object());
    flight_ids.push_back(
        writer.record(1, Tier::Flight, "bus.trigger", input, base::Json::object()));
    writer.record(1, Tier::Snapshot, "snapshot.mark", 0, base::Json::object());
    flight_ids.push_back(
        writer.record(2, Tier::Flight, "state.enter", flight_ids.back(), base::Json::object()));
}

std::vector<Record> read_all(const std::string& bundle) {
    auto opened = Reader::open(bundle);
    Reader& reader = unwrap(opened.reader);
    std::vector<Record> records;
    while (true) {
        auto next = reader.next();
        REQUIRE_FALSE(next.error.has_value());
        if (!next.record.has_value())
            break;
        records.push_back(std::move(*next.record));
    }
    return records;
}

} // namespace

TEST_CASE("journal.writer.bundle_layout_and_flight_always_on") {
    TempDir tmp("layout");
    auto opened = Writer::create(tmp.bundle("run"), pinned_config());
    Writer& writer = unwrap(opened.writer);

    // header.json exists BEFORE any record or close: the flight recorder is
    // on from construction (Zenith D026).
    const fs::path dir(tmp.bundle("run"));
    CHECK(fs::is_regular_file(dir / kHeaderFile));
    CHECK(fs::is_directory(dir / kSnapshotsDir));
    const std::string header_bytes = slurp(dir / kHeaderFile);
    CHECK(header_bytes.find("\"flight\":true") != std::string::npos);
    CHECK(header_bytes.find("created_at") == std::string::npos); // no wall clock by default

    // Creating over an existing path refuses.
    auto second = Writer::create(tmp.bundle("run"), pinned_config());
    CHECK(unwrap(second.error).code == "journal.bundle_exists");

    CHECK_FALSE(writer.close().has_value());
    CHECK(fs::is_regular_file(dir / kIndexFile));
}

TEST_CASE("journal.writer.dual_write_byte_identical") {
    TempDir tmp("dual");
    std::vector<std::uint64_t> ids_a;
    std::vector<std::uint64_t> ids_b;
    for (int run = 0; run < 2; ++run) {
        auto opened = Writer::create(tmp.bundle(run == 0 ? "a" : "b"), pinned_config());
        Writer& writer = unwrap(opened.writer);
        submit_script(writer, run == 0 ? ids_a : ids_b);
        CHECK_FALSE(writer.close().has_value());
    }
    CHECK(ids_a == ids_b);
    // Two independent writers, identical script -> identical bundle bytes.
    for (const std::string_view file : {kHeaderFile, kJournalFile, kIndexFile}) {
        const std::string a = slurp(fs::path(tmp.bundle("a")) / file);
        const std::string b = slurp(fs::path(tmp.bundle("b")) / file);
        CHECK(!a.empty());
        CHECK(a == b);
    }
}

TEST_CASE("journal.writer.tier_filtering_keeps_flight_bytes_invariant") {
    TempDir tmp("tiers");

    WriterConfig flight_only = pinned_config();
    WriterConfig full = pinned_config();
    full.tiers.snapshot = true;
    full.tiers.trace = true;

    std::vector<std::uint64_t> ids_flight;
    std::vector<std::uint64_t> ids_full;
    {
        auto opened = Writer::create(tmp.bundle("flight"), flight_only);
        Writer& writer = unwrap(opened.writer);
        submit_script(writer, ids_flight);
        CHECK_FALSE(writer.close().has_value());
    }
    {
        auto opened = Writer::create(tmp.bundle("full"), full);
        Writer& writer = unwrap(opened.writer);
        submit_script(writer, ids_full);
        CHECK_FALSE(writer.close().has_value());
    }

    // Filtered submissions still consume ids (D-BUILD-032): the flight
    // records carry the SAME ids under both configs...
    CHECK(ids_flight == ids_full);

    // ...and serialize to the same bytes — enabling trace/snapshot never
    // perturbs the causality skeleton.
    const std::vector<Record> flight_records = read_all(tmp.bundle("flight"));
    const std::vector<Record> full_records = read_all(tmp.bundle("full"));
    CHECK(flight_records.size() == 3);
    CHECK(full_records.size() == 5);
    std::vector<std::string> flight_lines;
    flight_lines.reserve(flight_records.size());
    for (const Record& record : flight_records)
        flight_lines.push_back(to_jsonl(record));
    std::vector<std::string> full_flight_lines;
    full_flight_lines.reserve(full_records.size());
    for (const Record& record : full_records)
        if (record.tier == Tier::Flight)
            full_flight_lines.push_back(to_jsonl(record));
    CHECK(flight_lines == full_flight_lines);
}

TEST_CASE("journal.writer.sticky_structured_errors") {
    TempDir tmp("errors");

    SUBCASE("tick regression poisons the writer") {
        auto opened = Writer::create(tmp.bundle("ticks"), pinned_config());
        Writer& writer = unwrap(opened.writer);
        CHECK(writer.record(5, Tier::Flight, "a", 0, base::Json::object()) == 1);
        CHECK(writer.record(4, Tier::Flight, "b", 0, base::Json::object()) == 0);
        CHECK(unwrap(writer.status()).code == "journal.tick_order");
        // Sticky: later records are refused, close surfaces the error and
        // writes no index (a poisoned bundle must not look valid).
        CHECK(writer.record(6, Tier::Flight, "c", 0, base::Json::object()) == 0);
        auto closed = writer.close();
        CHECK(unwrap(closed).code == "journal.tick_order");
        CHECK_FALSE(fs::exists(fs::path(tmp.bundle("ticks")) / kIndexFile));
    }

    SUBCASE("cause_id must already be assigned") {
        auto opened = Writer::create(tmp.bundle("cause"), pinned_config());
        Writer& writer = unwrap(opened.writer);
        CHECK(writer.record(0, Tier::Flight, "a", 1, base::Json::object()) == 0); // 1 not assigned
        CHECK(unwrap(writer.status()).code == "journal.cause_unknown");
    }

    SUBCASE("zero index stride is a config error") {
        WriterConfig config = pinned_config();
        config.index_stride_ticks = 0;
        auto opened = Writer::create(tmp.bundle("stride"), config);
        CHECK(unwrap(opened.error).code == "journal.config");
    }
}

TEST_CASE("journal.writer.flush_preserves_content") {
    TempDir tmp("flush");
    auto opened = Writer::create(tmp.bundle("flushed"), pinned_config());
    Writer& writer = unwrap(opened.writer);
    std::vector<std::uint64_t> ids;
    submit_script(writer, ids);
    CHECK_FALSE(writer.flush().has_value()); // crash-durability point
    CHECK(writer.record(2, Tier::Flight, "after.flush", 0, base::Json::object()) == 6);
    CHECK_FALSE(writer.close().has_value());

    const std::vector<Record> records = read_all(tmp.bundle("flushed"));
    REQUIRE(records.size() == 4);
    CHECK(records.back().kind == "after.flush");
}

TEST_CASE("journal.greppability") {
    // The journal stream is standard zstd frames with one JSON object per
    // line inside: `zstdcat journal.jsonl.zst | grep known_event` works.
    // This test pins the standard-frame magic and the line content; the
    // verify gate runs the real zstdcat over the committed twin of this
    // bundle (testkit/fixtures/journal/greppable.mrj) and byte-compares it
    // against a fresh regeneration (set MIDDAY_JOURNAL_FIXTURE_DIR).
    TempDir tmp("grep");
    const char* fixture_dir = std::getenv("MIDDAY_JOURNAL_FIXTURE_DIR");
    const std::string bundle =
        fixture_dir != nullptr ? std::string(fixture_dir) : tmp.bundle("greppable");

    {
        auto opened = Writer::create(bundle, pinned_config());
        Writer& writer = unwrap(opened.writer);
        base::Json payload = base::Json::object();
        payload.set("who", "greppability-fixture");
        const std::uint64_t root = writer.record(0, Tier::Flight, "input.key", 0, payload);
        const std::uint64_t hit =
            writer.record(0, Tier::Flight, "known_event", root, base::Json::object());
        writer.record(1, Tier::Flight, "state.enter", hit, base::Json::object());
        // Cross an index stride so the fixture pins the multi-frame shape.
        writer.record(300, Tier::Flight, "tick.marker", 0, base::Json::object());
        writer.record(301, Tier::Flight, "known_event", 0, base::Json::object());
        CHECK_FALSE(writer.close().has_value());
    }

    // Standard zstd frame magic: 0xFD2FB528, little-endian on disk.
    const std::string stream = slurp(fs::path(bundle) / kJournalFile);
    REQUIRE(stream.size() > 4);
    CHECK(static_cast<unsigned char>(stream[0]) == 0x28);
    CHECK(static_cast<unsigned char>(stream[1]) == 0xB5);
    CHECK(static_cast<unsigned char>(stream[2]) == 0x2F);
    CHECK(static_cast<unsigned char>(stream[3]) == 0xFD);

    // Two frames: the stride boundary at tick 300 cut a second one.
    auto opened = Reader::open(bundle);
    Reader& reader = unwrap(opened.reader);
    CHECK(reader.index().entries.size() == 2);

    // The equivalent of `zstdcat | grep known_event`.
    int known_events = 0;
    while (true) {
        auto next = reader.next();
        REQUIRE_FALSE(next.error.has_value());
        if (!next.record.has_value())
            break;
        if (next.record->kind == "known_event")
            ++known_events;
    }
    CHECK(known_events == 2);
}
