// loader.run / loader.diff — the two m0-yaml-loader-run verbs driven at the
// framework seam (parse_verb_args + spec.run — the exact path main() takes
// after argv splitting): the boss-shaped corpus runs headless with a FLIGHT
// bundle, authored-text refusals exit 3 (validation) with file/line
// details, same-seed dual runs diff identical, and different seeds report
// the exact first divergent tick (0 — the run.config root record).

#include "cli/verb.h"
#include "cli/verbs/test_support.h"
#include "core/base/file_io.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"

#include <string>
#include <vector>

using namespace midday;
using namespace midday::cli;
using midday::cli::testsupport::invoke;
using midday::testkit::unwrap;

namespace {

void write_corpus(const testkit::TempDir& dir, bool poison_machine = false) {
    REQUIRE_FALSE(base::write_file(dir.file("combat.events.yaml"),
                                   "format: 1\n"
                                   "events:\n"
                                   "  death.dealt: {payload: {}}\n"
                                   "  attack.swoosh: {payload: {}}\n",
                                   "t")
                      .has_value());
    std::string machine = "format: 1\n"
                          "machine: boss\n"
                          "regions:\n"
                          "  combat:\n"
                          "    initial: Passive\n"
                          "    anystate:\n"
                          "      - {event: death.dealt, goto: Dead, priority: 100}\n"
                          "    states:\n"
                          "      Passive: {}\n"
                          "      Dead: {}\n";
    if (poison_machine)
        machine += "      Ghost:\n        on:\n          - {event: death.dealt, goto: Nowhere}\n";
    REQUIRE_FALSE(base::write_file(dir.file("boss.machine.yaml"), machine, "t").has_value());
    REQUIRE_FALSE(base::write_file(dir.file("boss.scene.yaml"),
                                   "format: 1\n"
                                   "scene: arena\n"
                                   "events: [combat.events.yaml]\n"
                                   "entities:\n"
                                   "  - entity: Ground\n"
                                   "    components:\n"
                                   "      - Transform: {}\n"
                                   "      - Collider: {shape: plane}\n"
                                   "  - entity: Boss\n"
                                   "    components:\n"
                                   "      - Transform: {at: [0, 1.0, 0]}\n"
                                   "      - Collider: {shape: box, size: [1.2, 1.8, 1.2]}\n"
                                   "      - RigidBody: {}\n"
                                   "    machines:\n"
                                   "      - {instance: {path: boss.machine.yaml}}\n",
                                   "t")
                      .has_value());
}

} // namespace

TEST_CASE("loader.run: the corpus runs headless to the target tick, FLIGHT-recorded") {
    testkit::TempDir dir{"run-verb"};
    write_corpus(dir);
    const VerbOutcome out = invoke(run_spec(),
                                   {dir.file("boss.scene.yaml"),
                                    "--to-tick",
                                    "100",
                                    "--seed",
                                    "7",
                                    "--record",
                                    dir.file("a.mrj")});
    REQUIRE(out.exit == Exit::Ok);
    CHECK(out.payload.find("ticks")->as_int() == 100);
    CHECK(out.payload.find("recorded_tier")->as_string() == "flight");
    CHECK(out.payload.find("scene_name")->as_string() == "arena");
    CHECK(out.payload.find("entities")->as_int() == 2);
    CHECK(out.payload.find("machines")->as_int() == 1);
    CHECK(out.payload.find("bodies")->as_int() == 2);
    CHECK(out.payload.find("journal")->as_string() == dir.file("a.mrj"));

    // An explicit --record path is a recording: never clobbered.
    const VerbOutcome again = invoke(
        run_spec(), {dir.file("boss.scene.yaml"), "--ticks", "1", "--record", dir.file("a.mrj")});
    CHECK(again.exit == Exit::Failure);
    CHECK(unwrap(again.error).code == "journal.bundle_exists");
}

TEST_CASE("loader.run: unknown key and bad state ref exit 3 with file/line") {
    testkit::TempDir dir{"run-verb-bad"};
    write_corpus(dir);
    REQUIRE_FALSE(base::write_file(dir.file("typo.scene.yaml"),
                                   "format: 1\n"
                                   "scene: s\n"
                                   "entitiez:\n"
                                   "  - entity: E\n",
                                   "t")
                      .has_value());
    const VerbOutcome unknown = invoke(run_spec(), {dir.file("typo.scene.yaml")});
    CHECK(unknown.exit == Exit::Validation);
    CHECK(unwrap(unknown.error).code == "loader.unknown_key");
    CHECK(unwrap(unknown.error).details.find("line")->as_int() == 3);
    CHECK(unwrap(unknown.error).details.find("file")->as_string() == dir.file("typo.scene.yaml"));

    write_corpus(dir, /*poison_machine=*/true);
    const VerbOutcome bad_ref = invoke(run_spec(), {dir.file("boss.scene.yaml")});
    CHECK(bad_ref.exit == Exit::Validation);
    CHECK(unwrap(bad_ref.error).code == "loader.bad_ref");
    CHECK(unwrap(bad_ref.error).message.find("Nowhere") != std::string::npos);
    CHECK(unwrap(bad_ref.error).details.find("line")->as_int() == 13);
}

TEST_CASE("loader.run: --ticks and --to-tick are mutually exclusive") {
    testkit::TempDir dir{"run-verb-usage"};
    write_corpus(dir);
    const VerbOutcome out =
        invoke(run_spec(), {dir.file("boss.scene.yaml"), "--ticks", "1", "--to-tick", "2"});
    CHECK(out.exit == Exit::Usage);
    CHECK(unwrap(out.error).code == "usage.conflicting_flags");
}

TEST_CASE("loader.diff: same seed identical, different seed diverges at tick 0") {
    testkit::TempDir dir{"diff-verb"};
    write_corpus(dir);
    for (const char* name : {"a.mrj", "b.mrj", "c.mrj"}) {
        const bool other_seed = name[0] == 'c';
        const VerbOutcome out = invoke(run_spec(),
                                       {dir.file("boss.scene.yaml"),
                                        "--to-tick",
                                        "60",
                                        "--seed",
                                        other_seed ? "8" : "7",
                                        "--record",
                                        dir.file(name)});
        REQUIRE(out.exit == Exit::Ok);
    }

    const VerbOutcome same = invoke(journal_spec(), {"diff", dir.file("a.mrj"), dir.file("b.mrj")});
    CHECK(same.exit == Exit::Ok);
    CHECK(same.payload.find("identical")->as_bool());
    CHECK(same.payload.find("first_divergent_tick")->is_null());
    CHECK(same.payload.find("records_compared")->as_int() > 60);
    CHECK(same.payload.find("identity_a")->as_string() ==
          same.payload.find("identity_b")->as_string());

    const VerbOutcome diff = invoke(journal_spec(), {"diff", dir.file("a.mrj"), dir.file("c.mrj")});
    CHECK(diff.exit == Exit::Failure);
    CHECK(unwrap(diff.error).code == "journal.divergent");
    CHECK_FALSE(diff.payload.find("identical")->as_bool());
    CHECK(diff.payload.find("first_divergent_tick")->as_int() == 0);
    CHECK(diff.payload.find("divergence")->find("index")->as_int() == 0);

    const VerbOutcome unreadable =
        invoke(journal_spec(), {"diff", dir.file("a.mrj"), dir.file("ghost.mrj")});
    CHECK(unreadable.exit == Exit::Failure);

    const VerbOutcome bad_op =
        invoke(journal_spec(), {"frob", dir.file("a.mrj"), dir.file("b.mrj")});
    CHECK(bad_op.exit == Exit::Usage);
}
