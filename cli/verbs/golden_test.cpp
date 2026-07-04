// golden.appendix_a — the flagship golden (m0-appendix-a-determinism): the
// AUTHORED examples/appendix_a corpus driven to tick 3200 through the real
// run verb with --assert case=appendix_a_golden. Expected values here were
// derived from the MIDDAY_ENGINE_SPEC.md Appendix A.3 TEXT before the sim
// ran (test-first): if the sim and this file ever disagree, the sim (or the
// corpus) is wrong — never tune these to observed output.
//
// The five item-21 verdict names are the MILESTONE_0 exit-test contract;
// the journal spot checks below RE-DERIVE key facts with independent code
// so a defective assertion pack cannot vouch for itself.

#include "cli/verb.h"
#include "cli/verbs/test_support.h"
#include "core/journal/reader.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace midday;
using namespace midday::cli;
using midday::cli::testsupport::field;
using midday::cli::testsupport::invoke;
using midday::testkit::unwrap;

namespace {

constexpr const char* kScene = "examples/appendix_a/boss.scene.yaml";
constexpr const char* kCacheDir = ".midday-cache/selftest/golden";

VerbOutcome golden_run(const std::string& bundle) {
    return invoke(run_spec(),
                  {kScene,
                   "--to-tick",
                   "3200",
                   "--seed",
                   "7",
                   "--record",
                   bundle,
                   "--cache-dir",
                   kCacheDir,
                   "--assert",
                   "case=appendix_a_golden"});
}

} // namespace

TEST_CASE("golden.appendix_a: the 3200-tick driven run passes every A.3 verdict") {
    testkit::TempDir dir{"golden-a3"};
    const VerbOutcome out = golden_run(dir.file("a1.mrj"));
    REQUIRE(out.error.has_value() == false);
    REQUIRE(out.exit == Exit::Ok);
    CHECK(field(out.payload, "ticks").as_int() == 3200);
    CHECK(field(out.payload, "assert_case").as_string() == "appendix_a_golden");

    // Every named verdict, the five exit-test names first (item 21).
    const Json& assertions = field(out.payload, "assertions");
    CHECK(field(assertions, "combat_transitions_at_3200").as_int() == 1);
    for (const char* name : {"hurtbox_inactive_before_dead_enter",
                             "voided_stagger",
                             "locomotion_still_chasing",
                             "cause_chain_complete",
                             "slash_entered_at_e",
                             "swoosh_at_e18",
                             "span_open_at_e24",
                             "hurtbox_live_before_hit",
                             "span_closed_by_exit_chain",
                             "voided_hitboxlive_closed",
                             "exit_chain_order_a21",
                             "boss_died_broadcast",
                             "dead_state_active_at_end"}) {
        CAPTURE(name);
        CHECK(field(assertions, name).as_bool());
    }

    // TS hook seats made it into the envelope: the corpus scripts' hooks
    // are exactly what the modules declare (dead.ts: onEnter only;
    // slash_attack.ts: onEnter + onExit).
    const Json& scripts = field(out.payload, "scripts");
    REQUIRE(scripts.is_array());
    REQUIRE(scripts.elements().size() == 2);
    for (const Json& entry : scripts.elements()) {
        const std::string& state = field(entry, "state").as_string();
        const Json& hooks = field(entry, "hooks");
        REQUIRE(hooks.is_array());
        if (state == "combat/Dead") {
            REQUIRE(hooks.elements().size() == 1);
            CHECK(hooks.elements()[0].as_string() == "onEnter");
        } else {
            CHECK(state == "combat/SlashAttack");
            REQUIRE(hooks.elements().size() == 2);
            CHECK(hooks.elements()[0].as_string() == "onEnter");
            CHECK(hooks.elements()[1].as_string() == "onExit");
        }
    }

    // Independent journal spot checks (differently coded than the pack):
    // count the tick-3200 records by kind and pin the A.3 journal lines.
    journal::ReaderOpenResult opened = journal::Reader::open(dir.file("a1.mrj"));
    REQUIRE_FALSE(opened.error.has_value());
    journal::Reader& reader = unwrap(opened.reader);
    REQUIRE_FALSE(reader.seek_to_tick(3200).has_value());
    int combat_transitions = 0;
    int voids = 0;
    bool death_to_dead = false;
    bool stagger_voided = false;
    bool boss_died_global = false;
    bool span_close_via_exit = false;
    while (true) {
        journal::Reader::NextResult next = reader.next();
        REQUIRE_FALSE(next.error.has_value());
        if (!next.record.has_value())
            break;
        const journal::Record& record = *next.record;
        const std::string dumped = record.payload.dump();
        if (record.kind == "statechart.transition" &&
            dumped.find(R"("region":"combat")") != std::string::npos) {
            ++combat_transitions;
            death_to_dead = dumped.find(R"("from":"SlashAttack")") != std::string::npos &&
                            dumped.find(R"("to":"Dead")") != std::string::npos &&
                            dumped.find(R"("via":"any-state")") != std::string::npos;
        }
        if (record.kind == "statechart.voided") {
            ++voids;
            if (dumped.find(R"("event":"stagger.hit")") != std::string::npos)
                stagger_voided = dumped.find("region_already_transitioned") != std::string::npos;
        }
        if (record.kind == "event.trigger" &&
            dumped.find(R"("event":"boss.died")") != std::string::npos)
            boss_died_global = dumped.find(R"("key":"global")") != std::string::npos;
        if (record.kind == "sequence.span_close")
            span_close_via_exit = dumped.find(R"("via":"exit")") != std::string::npos;
    }
    CHECK(combat_transitions == 1); // A.3: "exactly one combat transition at t3200"
    CHECK(death_to_dead);
    CHECK(voids == 2); // stagger.hit + the mid-exit HitboxLive.closed echo
    CHECK(stagger_voided);
    CHECK(boss_died_global);
    CHECK(span_close_via_exit);
}

TEST_CASE("golden.appendix_a: two independent driven runs are journal-identical") {
    // Bit-identity is ALWAYS two independent runs diffed (Zenith N017).
    testkit::TempDir dir{"golden-a3-dual"};
    REQUIRE(golden_run(dir.file("a1.mrj")).exit == Exit::Ok);
    REQUIRE(golden_run(dir.file("a2.mrj")).exit == Exit::Ok);

    const VerbOutcome diff =
        invoke(journal_spec(), {"diff", dir.file("a1.mrj"), dir.file("a2.mrj")});
    CHECK(diff.exit == Exit::Ok);
    CHECK(field(diff.payload, "identical").as_bool());
    CHECK(field(diff.payload, "first_divergent_tick").is_null());
    CHECK(field(diff.payload, "records_compared").as_int() > 3200);
}

TEST_CASE("golden.appendix_a: --assert refuses malformed and unknown packs") {
    const VerbOutcome malformed = invoke(run_spec(), {kScene, "--ticks", "1", "--assert", "bogus"});
    CHECK(malformed.exit == Exit::Usage);
    CHECK(unwrap(malformed.error).code == "usage.bad_assert");

    const VerbOutcome unknown =
        invoke(run_spec(), {kScene, "--ticks", "1", "--assert", "case=nope"});
    CHECK(unknown.exit == Exit::Usage);
    CHECK(unwrap(unknown.error).code == "usage.unknown_assert_case");
    CHECK(unwrap(unknown.error).message.find("appendix_a_golden") != std::string::npos);
}
