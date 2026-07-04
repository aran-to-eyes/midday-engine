// spike.* — the m0-determinism-spike exit tests (MILESTONE_0 item 25). The
// kata scene must exercise TS GC churn, Jolt stepping, statechart cascades,
// and sequence spans PER TICK AND SIMULTANEOUSLY, reported as the envelope's
// `.exercised.*` booleans — asserted before any compare, so an empty-scene
// byte-compare can never vacuously pass. Expected activity floors are
// derived from the AUTHORED corpus (kata.machine.yaml header math), never
// tuned to observed output. Bit-identity is two INDEPENDENT runs compared
// (Zenith N017): the normalized record stream byte-for-byte AND the journal
// diff verb. The tainted variant proves the OTHER edge: Date.now() dies at
// the lint gate through the real run path before a single tick executes.

#include "cli/verb.h"
#include "cli/verbs/test_support.h"
#include "core/base/file_io.h"
#include "core/journal/reader.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

using namespace midday;
using namespace midday::cli;
using midday::cli::testsupport::field;
using midday::cli::testsupport::invoke;
using midday::testkit::unwrap;

namespace {

constexpr const char* kKataScene = "examples/spikes/determinism.scene.yaml";
constexpr const char* kTaintedScene = "examples/spikes/tainted/tainted.scene.yaml";
constexpr const char* kCacheDir = ".midday-cache/selftest/spike";

VerbOutcome kata_run(const std::string& bundle) {
    return invoke(run_spec(),
                  {kKataScene,
                   "--ticks",
                   "600",
                   "--seed",
                   "123",
                   "--record",
                   bundle,
                   "--cache-dir",
                   kCacheDir,
                   "--assert",
                   "case=determinism_kata"});
}

} // namespace

TEST_CASE("spike.determinism_kata: 600 ticks move all four exercised axes at once") {
    testkit::TempDir dir{"spike-kata"};
    const VerbOutcome out = kata_run(dir.file("ka.mrj"));
    REQUIRE(out.error.has_value() == false);
    REQUIRE(out.exit == Exit::Ok);
    CHECK(field(out.payload, "ticks").as_int() == 600);
    CHECK(field(out.payload, "assert_case").as_string() == "determinism_kata");

    // The item-25 exit-test contract: the four exercised booleans, exact
    // spellings, all true.
    const Json& exercised = field(out.payload, "exercised");
    for (const char* axis :
         {"ts_gc_churn", "jolt_step", "statechart_transitions", "sequence_spans"}) {
        CAPTURE(axis);
        CHECK(field(exercised, axis).as_bool());
    }
    CHECK(field(field(out.payload, "assertions"), "rng_streams_flowed").as_bool());

    // Independent journal spot checks (differently coded than the pack —
    // a defective pack cannot vouch for itself): count the raw record
    // stream against the authored corpus floors.
    journal::ReaderOpenResult opened = journal::Reader::open(dir.file("ka.mrj"));
    REQUIRE_FALSE(opened.error.has_value());
    journal::Reader& reader = unwrap(opened.reader);
    std::uint64_t transitions = 0;
    std::uint64_t span_opens = 0;
    std::uint64_t span_closes = 0;
    std::uint64_t contact_triggers = 0;
    std::uint64_t churn_emits = 0;
    std::set<std::string> churn_sums; // distinct serialized payloads = RNG variation
    while (true) {
        journal::Reader::NextResult next = reader.next();
        REQUIRE_FALSE(next.error.has_value());
        if (!next.record.has_value())
            break;
        const journal::Record& record = *next.record;
        if (record.kind == "statechart.transition")
            ++transitions;
        else if (record.kind == "sequence.span_open")
            ++span_opens;
        else if (record.kind == "sequence.span_close")
            ++span_closes;
        else if (record.kind == "event.trigger") {
            const std::string dumped = record.payload.dump();
            if (dumped.find(R"("event":"contact.began")") != std::string::npos)
                ++contact_triggers;
            else if (dumped.find(R"("event":"kata.churn")") != std::string::npos) {
                ++churn_emits;
                churn_sums.insert(dumped);
            }
        }
    }
    // 20 drive periods (30 ticks each): >= 7 transitions, 1 span pair, 24
    // churn emits per period; the floors sit near half of the design.
    CHECK(transitions >= 70);
    CHECK(span_opens >= 12);
    CHECK(span_closes >= 12);
    CHECK(contact_triggers >= 4); // agent + three debris must land
    CHECK(churn_emits >= 240);
    CHECK(churn_sums.size() >= 8); // seeded draws vary the journaled payloads
}

TEST_CASE("spike.determinism_kata: two independent runs are byte-identical") {
    // Bit-identity is ALWAYS two independent runs compared (Zenith N017):
    // the normalized record stream byte-for-byte, then the diff verb's
    // record-content walk naming any divergence.
    testkit::TempDir dir{"spike-kata-dual"};
    REQUIRE(kata_run(dir.file("ka.mrj")).exit == Exit::Ok);
    REQUIRE(kata_run(dir.file("kb.mrj")).exit == Exit::Ok);

    base::ReadFileResult a = base::read_file(dir.file("ka.mrj") + "/journal.jsonl.zst", "test.io");
    base::ReadFileResult b = base::read_file(dir.file("kb.mrj") + "/journal.jsonl.zst", "test.io");
    REQUIRE_FALSE(a.error.has_value());
    REQUIRE_FALSE(b.error.has_value());
    CHECK(a.bytes.size() > 0);
    CHECK(a.bytes == b.bytes);

    const VerbOutcome diff =
        invoke(journal_spec(), {"diff", dir.file("ka.mrj"), dir.file("kb.mrj")});
    CHECK(diff.exit == Exit::Ok);
    CHECK(field(diff.payload, "identical").as_bool());
    CHECK(field(diff.payload, "first_divergent_tick").is_null());
}

TEST_CASE("spike.tainted: Date.now() dies at the lint gate before any tick") {
    testkit::TempDir dir{"spike-tainted"};
    const VerbOutcome out = invoke(
        run_spec(),
        {kTaintedScene, "--ticks", "1", "--record", dir.file("t.mrj"), "--cache-dir", kCacheDir});
    CHECK(out.exit == Exit::Validation); // exit 3: authored-text refusal
    const base::Error& error = unwrap(out.error);
    CHECK(error.code == "script.lint");
    const Json& diagnostics = field(error.details, "diagnostics");
    REQUIRE(diagnostics.is_array());
    REQUIRE(diagnostics.elements().size() == 1);
    const Json& diag = diagnostics.elements()[0];
    CHECK(field(diag, "kind").as_string() == "lint");
    CHECK(field(diag, "code").as_string() == "no-wall-clock");
    CHECK(field(diag, "file").as_string().ends_with("tainted_clock.ts"));
    CHECK(field(diag, "line").as_int() > 0);
    // The refusal happened at BIND, pre-tick: a runtime script failure
    // would carry the sim-tick annotation (annotate_sim_context) — its
    // absence pins that no tick ever executed.
    CHECK(error.details.find("tick") == nullptr);
}

TEST_CASE("spike.registry: determinism_kata is a registered assert pack") {
    const VerbOutcome unknown =
        invoke(run_spec(), {kKataScene, "--ticks", "1", "--assert", "case=nope"});
    CHECK(unknown.exit == Exit::Usage);
    CHECK(unwrap(unknown.error).message.find("determinism_kata") != std::string::npos);
}
