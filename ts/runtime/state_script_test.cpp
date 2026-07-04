// golden.ts_hook_parity — TS state scripts through the generated hook seam
// drive the SAME A.2.1 semantics the C++ fixtures pinned at
// m0-statechart-core (MILESTONE_0 "Statechart semantics are proven
// C++-first": core semantics and binding fidelity are two separately
// falsifiable claims — this file is the second one).
//
// Method: the hook_order_test machine shape (Passive; SlashAttack > Strike >
// Deep; Dead; any-state death.dealt prio 100) runs twice under identical
// drives — once with C++ StateHooks emitting a probe event from every hook,
// once with TS state scripts emitting the SAME probe through __midday_emit
// on QuickJS. The two journals must match RECORD FOR RECORD (and as raw
// compressed bytes): same hook records, same probe triggers, same cause ids.
// The probe sequence is additionally pinned against the normative A.2.1
// list spelled out in hook_order_test.cpp — two independently produced
// streams agreeing with each other AND with the text, never a self-diff.

#include "core/base/file_io.h"
#include "core/statechart/test_support.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "testkit/temp_dir.h"
#include "ts/runtime/state_script.h"
#include "ts/toolchain/toolchain.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace midday;
using namespace midday::statechart;
using namespace midday::statechart::test;
using midday::base::Json;
using midday::base::Name;
using midday::journal::Record;
using midday::testkit::unwrap;

namespace {

// The machine shape of statechart.hook_order (the C++-pinned fixture).
MachineDesc nested_machine() {
    StateDesc deep = state("Deep");
    StateDesc strike = state("Strike");
    strike.substates.push_back(deep);
    strike.initial = Name("Deep");
    StateDesc slash = state("SlashAttack");
    slash.substates.push_back(strike);
    slash.initial = Name("Strike");
    return machine("boss_brain",
                   {region("combat",
                           "Passive",
                           {state("Passive", {pair("attack", "Strike")}), slash, state("Dead")},
                           {pair("death.dealt", "Dead", 100)})});
}

constexpr const char* kProbeStates[] = {"Passive", "SlashAttack", "Strike", "Deep", "Dead"};

// The C++ probe: every hook emits probe.hook {state, hook, peer} @ global,
// cause = the hook's own journal record — byte-for-byte what the TS scripts
// below emit through __midday_emit.
struct ParityProbeHooks final : StateHooks {
    bus::Bus* bus = nullptr;

    void emit(const StateHookContext& ctx, const char* hook, std::string peer) {
        Json payload = Json::object();
        payload.set("state", std::string(ctx.state.view()));
        payload.set("hook", hook);
        payload.set("peer", std::move(peer));
        REQUIRE_FALSE(
            bus->trigger(
                   bus::EventKey::named(Name("global")), Name("probe.hook"), payload, ctx.record_id)
                .error.has_value());
    }

    void on_enter(Statechart&, const StateHookContext& ctx) override {
        emit(ctx, "enter", std::string(ctx.peer.view()));
    }

    void on_exit(Statechart&, const StateHookContext& ctx) override {
        emit(ctx, "exit", std::string(ctx.peer.view()));
    }

    void on_update(Statechart&, const StateHookContext& ctx) override { emit(ctx, "update", ""); }

    void on_fixed_update(Statechart&, const StateHookContext& ctx) override {
        emit(ctx, "fixed", "");
    }
};

// One TS probe script per state — the exact TS mirror of ParityProbeHooks.
std::string probe_script(const char* state_name) {
    const std::string name(state_name);
    return "declare function __midday_emit(event: string, payload: unknown, key: string): number\n"
           "\n"
           "export default class Probe {\n"
           "    onEnter(from: string): void {\n"
           "        __midday_emit(\"probe.hook\", { state: \"" +
           name +
           "\", hook: \"enter\", peer: from }, \"global\")\n"
           "    }\n"
           "    onExit(to: string): void {\n"
           "        __midday_emit(\"probe.hook\", { state: \"" +
           name +
           "\", hook: \"exit\", peer: to }, \"global\")\n"
           "    }\n"
           "    onUpdate(dt: number): void {\n"
           "        void dt\n"
           "        __midday_emit(\"probe.hook\", { state: \"" +
           name +
           "\", hook: \"update\", peer: \"\" }, \"global\")\n"
           "    }\n"
           "    onFixedUpdate(dt: number): void {\n"
           "        void dt\n"
           "        __midday_emit(\"probe.hook\", { state: \"" +
           name +
           "\", hook: \"fixed\", peer: \"\" }, \"global\")\n"
           "    }\n"
           "}\n";
}

// loader::resolve_key's shape without the loader dependency (spec 4.2).
bus::EventKey test_resolve_key(std::string_view spelling, ecs::EntityRef host) {
    if (spelling == "self" || spelling == "root")
        return bus::EventKey::entity(host);
    return bus::EventKey::named(Name(spelling));
}

script::ToolchainConfig fresh_toolchain(const std::string& name) {
    script::ToolchainConfig config;
    config.cache_dir = ".midday-cache/selftest/" + name;
    std::filesystem::remove_all(config.cache_dir);
    return config;
}

// The identical drive for both runs: deep entry, one fixed tick, one frame
// update, then the any-state death rule.
void drive(ChartFixture& fix) {
    REQUIRE_FALSE(fix.trigger("attack").error.has_value());
    REQUIRE_FALSE(fix.loop().tick().has_value());
    fix.chart().run_update(1.0 / 120.0, 0);
    REQUIRE_FALSE(fix.trigger("death.dealt").error.has_value());
}

// probe.hook triggers as "<hook>:<state>:<peer>" strings, journal order.
std::vector<std::string> probe_sequence(const std::vector<Record>& records) {
    std::vector<std::string> out;
    for (const Record& record : records) {
        if (record.kind != "event.trigger")
            continue;
        if (field(record.payload, "event").as_string() != "probe.hook")
            continue;
        const Json& payload = field(record.payload, "payload");
        out.push_back(field(payload, "hook").as_string() + ":" +
                      field(payload, "state").as_string() + ":" +
                      field(payload, "peer").as_string());
    }
    return out;
}

} // namespace

TEST_CASE("golden.ts_hook_parity: TS scripts reproduce the C++-driven journal record for record") {
    // ---- run A: C++ hooks ---------------------------------------------------
    ChartFixture fix_a;
    ParityProbeHooks hooks;
    hooks.bus = &fix_a.bus();
    const MachineId id_a = fix_a.spawn_machine(nested_machine());
    for (const char* name : kProbeStates)
        REQUIRE_FALSE(
            fix_a.chart().set_state_hooks(id_a, Name("combat"), Name(name), hooks).has_value());
    drive(fix_a);
    const std::vector<Record> records_a = fix_a.finish();

    // ---- run B: TS scripts through the generated seam ----------------------
    ChartFixture fix_b;
    testkit::TempDir scripts_dir{"state-script-parity"};
    script::Toolchain toolchain(fresh_toolchain("state-script-parity"));
    script::ScriptRuntime runtime; // SIM profile
    script::StateScriptHost host(runtime, toolchain, fix_b.bus(), &test_resolve_key);
    REQUIRE_FALSE(host.first_error().has_value());
    const MachineId id_b = fix_b.spawn_machine(nested_machine());
    for (const char* name : kProbeStates) {
        const std::string path = scripts_dir.file(std::string(name) + ".ts");
        REQUIRE_FALSE(base::write_file(path, probe_script(name), "t").has_value());
        REQUIRE_FALSE(host.bind(fix_b.chart(), id_b, Name("combat"), Name(name), fix_b.host, path)
                          .has_value());
    }
    CHECK(host.seat_count() == 5);
    drive(fix_b);
    REQUIRE_FALSE(host.first_error().has_value());
    const std::vector<Record> records_b = fix_b.finish();

    // ---- the parity pin: record-for-record equality -------------------------
    REQUIRE(records_a.size() == records_b.size());
    for (std::size_t i = 0; i < records_a.size(); ++i) {
        CAPTURE(i);
        CAPTURE(records_a[i].kind);
        CHECK(records_a[i].tick == records_b[i].tick);
        CHECK(records_a[i].tier == records_b[i].tier);
        CHECK(records_a[i].kind == records_b[i].kind);
        CHECK(records_a[i].cause_id == records_b[i].cause_id);
        CHECK(records_a[i].id == records_b[i].id);
        CHECK(records_a[i].payload.dump() == records_b[i].payload.dump());
    }

    // Raw compressed journals byte-equal too (same records, same cadence).
    base::ReadFileResult bytes_a = base::read_file(
        (std::filesystem::path(fix_a.bundle_path()) / "journal.jsonl.zst").string(), "t");
    base::ReadFileResult bytes_b = base::read_file(
        (std::filesystem::path(fix_b.bundle_path()) / "journal.jsonl.zst").string(), "t");
    REQUIRE_FALSE(bytes_a.error.has_value());
    REQUIRE_FALSE(bytes_b.error.has_value());
    CHECK(bytes_a.bytes.size() > 0);
    CHECK(bytes_a.bytes == bytes_b.bytes);

    // ---- and both agree with the NORMATIVE A.2.1 text (never dual-wrong) ----
    // Exit scripts outer->inner with peer = the transition target; enter
    // scripts inner->outer with peer = the exited top state; update flavors
    // in scene-tree depth-first order (hook_order_test's pinned lists).
    const std::vector<std::string> normative = {
        "exit:Passive:Strike", // deep-target entry: brain first
        "enter:Deep:Passive",  // mirror — inner->outer
        "enter:Strike:Passive",
        "enter:SlashAttack:Passive",
        "fixed:SlashAttack:", // phase 5, parents before substates
        "fixed:Strike:",
        "fixed:Deep:",
        "update:SlashAttack:", // frame-side flavor, same order
        "update:Strike:",
        "update:Deep:",
        "exit:SlashAttack:Dead", // any-state death: outer->inner
        "exit:Strike:Dead",
        "exit:Deep:Dead",
        "enter:Dead:SlashAttack",
    };
    CHECK(probe_sequence(records_a) == normative);
    CHECK(probe_sequence(records_b) == normative);
}

TEST_CASE("golden.ts_hook_parity: seam constants match the committed bindings_spec artifact") {
    // The generated seam is DATA (api/bindings_spec.json state_script_hooks,
    // spec section 7: no hand-written bindings drift). This pins the C++
    // constants against the committed artifact — edit one side and this
    // fails until both agree.
    base::ReadFileResult bytes = base::read_file("api/bindings_spec.json", "t");
    REQUIRE_FALSE(bytes.error.has_value());
    Json::ParseResult parsed = Json::parse(bytes.bytes, "bindings_spec.json");
    REQUIRE_FALSE(parsed.error.has_value());
    const Json* seam = parsed.value.find("state_script_hooks");
    REQUIRE(seam != nullptr);
    CHECK(field(*seam, "envelope_version").as_int() == script::StateScriptHost::kEnvelopeVersion);
    CHECK(field(*seam, "register").as_string() == script::StateScriptHost::kRegisterFn);
    CHECK(field(*seam, "introspect").as_string() == script::StateScriptHost::kIntrospectFn);
    CHECK(field(*seam, "invoke").as_string() == script::StateScriptHost::kInvokeFn);
    CHECK(field(*seam, "emit").as_string() == script::StateScriptHost::kEmitFn);
    const Json& hooks = field(*seam, "hooks");
    REQUIRE(hooks.is_array());
    REQUIRE(hooks.elements().size() == 4);
    for (std::size_t i = 0; i < 4; ++i)
        CHECK(hooks.elements()[i].as_string() == script::StateScriptHost::kHookNames[i]);
}

TEST_CASE("golden.ts_hook_parity: absent hooks never cross; hook faults surface structured") {
    ChartFixture fix;
    testkit::TempDir scripts_dir{"state-script-faults"};
    script::Toolchain toolchain(fresh_toolchain("state-script-faults"));
    script::ScriptRuntime runtime;
    script::StateScriptHost host(runtime, toolchain, fix.bus(), &test_resolve_key);

    MachineDesc desc = machine(
        "faulty", {region("main", "Idle", {state("Idle", {pair("go", "Boom")}), state("Boom")})});
    const MachineId id = fix.spawn_machine(desc);

    // Enter-only class: onExit/onUpdate/onFixedUpdate never cross the seam.
    const std::string quiet = scripts_dir.file("quiet.ts");
    REQUIRE_FALSE(base::write_file(quiet,
                                   "export default class Quiet {\n"
                                   "    onEnter(from: string): void {\n"
                                   "        void from\n"
                                   "    }\n"
                                   "}\n",
                                   "t")
                      .has_value());
    REQUIRE_FALSE(
        host.bind(fix.chart(), id, Name("main"), Name("Idle"), fix.host, quiet).has_value());

    // A throwing onEnter: captured as the first structured error, tick-
    // annotated, without unwinding the transition (the machine still lands).
    const std::string boom = scripts_dir.file("boom.ts");
    REQUIRE_FALSE(base::write_file(boom,
                                   "export default class Boom {\n"
                                   "    onEnter(from: string): void {\n"
                                   "        throw new Error(\"boom in \" + from)\n"
                                   "    }\n"
                                   "}\n",
                                   "t")
                      .has_value());
    REQUIRE_FALSE(
        host.bind(fix.chart(), id, Name("main"), Name("Boom"), fix.host, boom).has_value());
    CHECK(host.seat_count() == 2);

    const std::uint64_t gas_before = runtime.gas_used();
    REQUIRE_FALSE(fix.loop().tick().has_value()); // Idle active: no update hooks cross
    CHECK(runtime.gas_used() == gas_before);
    REQUIRE_FALSE(host.first_error().has_value());

    REQUIRE_FALSE(fix.trigger("go").error.has_value());
    CHECK(fix.chart().in_state(id, Name("main"), Name("Boom")));
    const base::Error& fault = unwrap(host.first_error());
    CHECK(fault.code == "script.exception");
    CHECK(fault.message.find("boom in Idle") != std::string::npos);
    CHECK(field(fault.details, "hook").as_string() == "onEnter");
    CHECK(field(fault.details, "tick").as_int() == 1);

    (void)fix.finish();
}
