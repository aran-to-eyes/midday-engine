// golden.component_event_lifecycle — the M2 0B golden (FUSED-SPEC D6): the
// authored examples/lifecycle corpus driven 241 ticks through the real run
// verb with --components + --assert case=component_event_lifecycle.
// Expected values derive from the DESIGN (the codec's pinned v1 layout, the
// A.2.1 exit template, 1 + ceil(4.0 * 60) = 241) — never from observed
// output. The journal spot checks below RE-DERIVE key facts with
// independent code so a defective assertion pack cannot vouch for itself.

#include "cli/verb.h"
#include "cli/verbs/test_support.h"
#include "testkit/doctest.h"
#include "testkit/temp_dir.h"

#include <string>
#include <vector>

using namespace midday;
using namespace midday::cli;
using midday::cli::testsupport::field;
using midday::cli::testsupport::for_each_record;
using midday::cli::testsupport::invoke;

namespace {

constexpr const char* kScene = "examples/lifecycle/lifecycle.scene.yaml";
constexpr const char* kManifest = "examples/lifecycle/lifecycle.components.json";
constexpr const char* kCacheDir = ".midday-cache/selftest/golden_lifecycle";

VerbOutcome lifecycle_run(const std::string& bundle) {
    return invoke(run_spec(),
                  {kScene,
                   "--ticks",
                   "241",
                   "--seed",
                   "7",
                   "--record",
                   bundle,
                   "--components",
                   kManifest,
                   "--cache-dir",
                   kCacheDir,
                   "--assert",
                   "case=component_event_lifecycle"});
}

} // namespace

TEST_CASE("golden.component_event_lifecycle: the 241-tick driven run passes every D6 verdict") {
    testkit::TempDir dir{"golden-lifecycle"};
    const VerbOutcome out = lifecycle_run(dir.file("l1.mrj"));
    REQUIRE_MESSAGE(out.error.has_value() == false,
                    (out.error ? out.error->message : std::string()));
    REQUIRE(out.exit == Exit::Ok);
    CHECK(field(out.payload, "ticks").as_int() == 241);
    CHECK(field(out.payload, "assert_case").as_string() == "component_event_lifecycle");

    const Json& assertions = field(out.payload, "assertions");
    for (const char* name : {"initial_entry_seated_order",
                             "exit_chain_seven_lines",
                             "contact_payload_bytes_pinned",
                             "canonical_payloads_verified",
                             "typed_hydration_verified",
                             "signed_zero_normalized",
                             "base_transform_mirror_read",
                             "kill_cause_chain",
                             "boss_died_at_enter_once",
                             "despawn_scheduled_due_241",
                             "despawn_exit_order",
                             "reaped_at_exactly_241"}) {
        CAPTURE(name);
        CHECK(field(assertions, name).as_bool());
    }
    CHECK(field(assertions, "boss_died_count").as_int() == 1);
    CHECK(field(assertions, "enveloped_triggers").as_int() ==
          field(assertions, "envelope_verified").as_int());

    // The three script seats made it into the envelope with exactly the
    // hooks the modules declare — the REAL warden dead.ts is onEnter-only.
    const Json& scripts = field(out.payload, "scripts");
    REQUIRE(scripts.is_array());
    REQUIRE(scripts.elements().size() == 3);
    for (const Json& entry : scripts.elements()) {
        const std::string& state = field(entry, "state").as_string();
        const Json& hooks = field(entry, "hooks");
        REQUIRE(hooks.is_array());
        if (state == "life/Dead") {
            CHECK(field(entry, "module").as_string() == "examples/warden/states/dead.ts");
            REQUIRE(hooks.elements().size() == 1);
            CHECK(hooks.elements()[0].as_string() == "onEnter");
        } else {
            REQUIRE(hooks.elements().size() == 2);
            CHECK(hooks.elements()[0].as_string() == "onEnter");
            CHECK(hooks.elements()[1].as_string() == "onExit");
        }
    }

    // Independent journal spot checks (differently coded than the pack):
    // the tick-1 hook chain in journal order, the literal canonical bytes,
    // the despawn arithmetic, and the one-boss.died rule.
    std::vector<std::string> tick1_hooks;
    int boss_died = 0;
    bool contact_bytes = false;
    bool despawn_due_241 = false;
    bool despawned_at_241 = false;
    for_each_record(dir.file("l1.mrj"), [&](const journal::Record& record) {
        const std::string dumped = record.payload.dump();
        if (record.kind == "statechart.hook" && record.tick == 1) {
            std::string line = field(record.payload, "hook").as_string();
            if (const Json* component = record.payload.find("component"))
                line += ":" + component->as_string();
            else
                line += ":" + field(record.payload, "state").as_string();
            tick1_hooks.push_back(line);
        }
        if (record.kind == "event.trigger") {
            // The OUTER event name (a dump search would also match nested
            // payload fields — relay.verify echoes "contact.began" inside).
            const std::string& outer = field(record.payload, "event").as_string();
            if (outer == "boss.died")
                ++boss_died;
            if (outer == "contact.began") {
                // The canonical-bytes spot check, independently re-derived
                // from the pinned v1 layout: the envelope header (version +
                // contact.began's compat hash LE + field count + presence),
                // the refs (self 0#0, other 6#0), and the -0-sensitive tail
                // (position [7, +0, 3] then normal + impulse +0 closing the
                // hex string) — the signed-zero falsifier reds HERE too.
                contact_bytes =
                    dumped.find(R"("payload_bytes":"0156635c241685d608050000001f)"
                                R"(0b00000000000000000b0600000000000000)") != std::string::npos &&
                    dumped.find("070000000000001c40000000000000000000000000000008"
                                "40") != std::string::npos &&
                    dumped.find("030000000000000000\"") != std::string::npos;
            }
            if (outer == "entity.despawned")
                despawned_at_241 = record.tick == 241;
        }
        if (record.kind == "prefab.despawn")
            despawn_due_241 = record.tick == 241 &&
                              dumped.find(R"("requested":1)") != std::string::npos &&
                              dumped.find(R"("due":241)") != std::string::npos;
    });
    const std::vector<std::string> expected_chain = {"exit:Alive",
                                                     "exit:Armed",
                                                     "component_exit:ChildExitB",
                                                     "component_exit:ChildExitA",
                                                     "component_exit:ParentExitB",
                                                     "component_exit:ParentExitA",
                                                     "enter:Dead"};
    CHECK(tick1_hooks == expected_chain); // THE 7-line exit order, re-derived
    CHECK(boss_died == 1);
    CHECK(contact_bytes);
    CHECK(despawn_due_241);
    CHECK(despawned_at_241);
}

TEST_CASE("golden.component_event_lifecycle: two independent driven runs are journal-identical") {
    // Bit-identity is ALWAYS two independent runs diffed (Zenith N017).
    testkit::TempDir dir{"golden-lifecycle-dual"};
    REQUIRE(lifecycle_run(dir.file("l1.mrj")).exit == Exit::Ok);
    REQUIRE(lifecycle_run(dir.file("l2.mrj")).exit == Exit::Ok);

    const VerbOutcome diff =
        invoke(journal_spec(), {"diff", dir.file("l1.mrj"), dir.file("l2.mrj")});
    CHECK(diff.exit == Exit::Ok);
    CHECK(field(diff.payload, "identical").as_bool());
    CHECK(field(diff.payload, "first_divergent_tick").is_null());
}
