// `midday run <scene> [--ticks N | --to-tick N] [--seed S] [--record path]
// [--assert case=<pack>]` — the headless sim runner (m0-yaml-loader-run):
// authored YAML -> loader -> World/Hierarchy/Bus/TickLoop/Statechart
// (+ physics when the scene references it, + the TS tier when state
// scripts are referenced: modules SEATED via ts/runtime/state_script.h,
// hooks live) -> fixed ticks -> a FLIGHT run.mrj bundle. Recording is ON
// BY CONSTRUCTION (Zenith D026): there is no run without a journal —
// omitting --record writes the scratch bundle .midday-cache/run/last.mrj
// (recreated per run; an EXPLICIT --record path never clobbers an existing
// bundle). --assert rides a registered assertion pack on the run
// (cli/verbs/run_assert.h): driver systems + named journal/live verdicts
// in the envelope's `.assertions`; any failed verdict exits 1.
//
// Exit contract: 3 (validation) for every authored-text refusal (yaml.*,
// loader.*, script diagnostics — all with file:line), 1 for runtime/journal
// failures, 2 for usage. The envelope reports {ok, ticks, recorded_tier,
// journal, seed, scene, counts...} — the node's exit test asserts on it.

#include "cli/verb.h"
#include "cli/verbs/run_assert.h"
#include "core/bus/bus.h"
#include "core/ecs/world.h"
#include "core/hierarchy/hierarchy.h"
#include "core/journal/writer.h"
#include "core/loader/loader.h"
#include "core/math/rng.h"
#include "core/physics/physics_server.h"
#include "core/reflect/builtin_events.h"
#include "core/reflect/registry.h"
#include "core/statechart/statechart.h"
#include "core/tick/tick_loop.h"
#include "ts/runtime/script_runtime.h"
#include "ts/runtime/state_script.h"
#include "ts/toolchain/toolchain.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#ifndef MIDDAY_VERSION
#define MIDDAY_VERSION "0.0.0-unversioned"
#endif

namespace midday::cli {
namespace {

constexpr std::string_view kScratchBundle = ".midday-cache/run/last.mrj";

// Authored-text refusals are the validation class; everything the loader
// and YAML layer emit carries file:line:col already. script.* splits: the
// toolchain's diagnostics (type/lint/module resolution) are authored-text,
// but a script FAILING AT RUNTIME is a runtime failure (exit 1 — the
// script-verb runtime_throw precedent).
bool is_validation(const Error& error) {
    if (error.code == "script.exception" || error.code == "script.interrupted" ||
        error.code == "script.out_of_memory" || error.code == "script.emit_outside_hook")
        return false;
    return error.code.starts_with("loader.") || error.code.starts_with("yaml.") ||
           error.code.starts_with("script.");
}

VerbOutcome refuse(Error error) {
    VerbOutcome out;
    out.exit = is_validation(error) ? Exit::Validation : Exit::Failure;
    out.error = std::move(error);
    return out;
}

// __midday_rng(stream): seeded, NAMED Philox child streams for state
// scripts — the SIM profile's deterministic replacement for the poisoned
// Math.random. Derivation: run seed -> child("scripts") -> child(<stream>);
// each named stream persists for the run, so per-tick draws advance a pure
// (seed, stream, draw-count) function of the executed program. The CLI owns
// this ambient sim service until the API tier formalizes RNG bindings.
void register_script_rng(script::ScriptRuntime& runtime, std::uint64_t seed) {
    struct Streams {
        math::RngStream root;
        std::map<std::string, math::RngStream> named;
    };

    auto streams = std::make_shared<Streams>(
        Streams{.root = math::RngStream(seed).child(base::Name("scripts")), .named = {}});
    runtime.register_host_fn("__midday_rng", [streams](const Json::Array& args) {
        script::HostResult result;
        if (args.size() != 1 || !args[0].is_string()) {
            result.error = Error{.code = "script.rng_args",
                                 .message = "__midday_rng expects (stream: string)"};
            return result;
        }
        const std::string& name = args[0].as_string();
        auto [it, fresh] = streams->named.try_emplace(name, streams->root.child(base::Name(name)));
        (void)fresh;
        result.value = Json(it->second.uniform_double());
        return result;
    });
}

// The canonical sim composition (destruction order = reverse declaration:
// chart detaches its hooks first, then physics; the script host outlives
// the chart per the StateHooks lifetime contract; the assert pack detaches
// its driver hook/subscription before the loop and bus die).
struct RunSim {
    reflect::Registry registry;
    ecs::World world{registry};
    hierarchy::Hierarchy hierarchy{world};
    std::optional<journal::Writer> writer;
    std::optional<bus::Bus> bus;
    std::optional<tick::TickLoop> loop;
    std::unique_ptr<RunAssertPack> pack;
    std::optional<script::Toolchain> toolchain;
    std::optional<script::ScriptRuntime> runtime;
    std::optional<script::StateScriptHost> scripts;
    std::unique_ptr<physics::PhysicsServer> physics;
    std::optional<statechart::Statechart> chart;
};

VerbOutcome run_verb(const VerbArgs& args) {
    const std::string& scene_path = args.get_string("scene");
    const std::int64_t seed = args.present("seed") ? args.get_int("seed") : 0;
    const bool has_ticks = args.present("ticks");
    const bool has_to_tick = args.present("to-tick");
    if (has_ticks && has_to_tick) {
        VerbOutcome out;
        out.exit = Exit::Usage;
        out.error = Error{.code = "usage.conflicting_flags",
                          .message = "--ticks and --to-tick are mutually exclusive"};
        return out;
    }

    // --assert case=<name>: a registered assertion pack rides the run.
    std::string assert_case;
    if (args.present("assert")) {
        const std::string& raw = args.get_string("assert");
        constexpr std::string_view kCasePrefix = "case=";
        if (!raw.starts_with(kCasePrefix) || raw.size() == kCasePrefix.size()) {
            VerbOutcome out;
            out.exit = Exit::Usage;
            out.error = Error{
                .code = "usage.bad_assert",
                .message = "--assert expects case=<name> (available: " + assert_pack_names() + ")"};
            return out;
        }
        assert_case = raw.substr(kCasePrefix.size());
    }

    RunSim sim;
    reflect::register_builtin_events(sim.registry);

    loader::SceneLoadResult loaded = loader::load_scene(scene_path, sim.registry);
    if (loaded.error.has_value() || !loaded.scene.has_value())
        return refuse(std::move(loaded.error)
                          .value_or(Error{.code = "loader.io", .message = "scene load failed"}));
    loader::SceneFile& scene = *loaded.scene;

    // Bundle path: explicit --record is a real recording (never clobbered);
    // the scratch default recreates per run.
    const bool scratch = !args.present("record");
    const std::string bundle = scratch ? std::string(kScratchBundle) : args.get_string("record");
    std::error_code ec;
    if (scratch)
        std::filesystem::remove_all(bundle, ec);
    const std::filesystem::path parent = std::filesystem::path(bundle).parent_path();
    if (!parent.empty())
        std::filesystem::create_directories(parent, ec);

    journal::WriterConfig config;
    config.engine_version = MIDDAY_VERSION;
    config.seed = static_cast<std::uint64_t>(seed);
    journal::WriterOpenResult opened = journal::Writer::create(bundle, config);
    if (opened.error.has_value() || !opened.writer.has_value())
        return refuse(
            std::move(opened.error)
                .value_or(Error{.code = "journal.io", .message = "bundle create failed"}));
    sim.writer.emplace(std::move(*opened.writer));

    sim.bus.emplace(sim.world, sim.registry, *sim.writer);
    sim.loop.emplace(sim.world, sim.hierarchy, *sim.bus, *sim.writer);

    // The assert pack's driver attaches BEFORE the Statechart: its phase-5
    // systems run first within the phase (A.1: pre-update systems, then
    // onFixedUpdate hooks — where A.3 places the damage system).
    if (!assert_case.empty()) {
        sim.pack = make_assert_pack(assert_case);
        if (sim.pack == nullptr) {
            VerbOutcome out;
            out.exit = Exit::Usage;
            out.error = Error{.code = "usage.unknown_assert_case",
                              .message = "unknown assertion pack '" + assert_case +
                                         "' (available: " + assert_pack_names() + ")"};
            return out;
        }
        if (auto error =
                sim.pack->attach(sim.world, sim.hierarchy, *sim.bus, *sim.loop, *sim.writer))
            return refuse(std::move(*error));
    }

    sim.chart.emplace(sim.world, sim.hierarchy, *sim.bus, *sim.writer, *sim.loop);

    // The run's ROOT record: scene + seed + rate are the run's identity in
    // the causality stream itself (different seeds diverge at tick 0 by
    // construction — the journal-diff exit test rides on this record).
    base::Json run_config = base::Json::object();
    run_config.set("scene", scene_path);
    run_config.set("seed", seed);
    run_config.set("ticks_per_second", sim.loop->ticks_per_second());
    const std::uint64_t cause =
        sim.writer->record(0, journal::Tier::Flight, "run.config", 0, std::move(run_config));
    if (cause == 0)
        return refuse(sim.writer->status().value_or(
            Error{.code = "journal.refused", .message = "run.config record refused"}));

    if (auto error = loader::register_scene_events(scene, sim.registry))
        return refuse(std::move(*error));

    if (loader::scene_uses_physics(scene)) {
        physics::PhysicsServerCreateResult created =
            physics::PhysicsServer::create(sim.world, sim.hierarchy, *sim.bus);
        if (created.error.has_value())
            return refuse(std::move(*created.error));
        sim.physics = std::move(created.server);
        if (auto error = sim.loop->add_hook(tick::Phase::kPhysics, *sim.physics))
            return refuse(std::move(*error));
    }

    loader::SpawnResult spawned = loader::spawn_scene(
        scene, sim.world, sim.hierarchy, *sim.chart, sim.physics.get(), *sim.writer, cause);
    if (spawned.error.has_value())
        return refuse(std::move(*spawned.error));

    if (sim.pack != nullptr)
        if (auto error = sim.pack->bind(*sim.chart, spawned, cause))
            return refuse(std::move(*error));

    // State scripts: build through the content-hash cache (typecheck + the
    // engine lint pack), instantiate on the SIM runtime, and SEAT on their
    // states — onEnter/onExit/onUpdate/onFixedUpdate run through the
    // generated hook seam (ts/runtime/state_script.h, A.2.1 parity).
    Json scripts = Json::array();
    if (!spawned.scripts.empty()) {
        script::ToolchainConfig tool_config;
        if (args.present("cache-dir"))
            tool_config.cache_dir = args.get_string("cache-dir");
        sim.toolchain.emplace(std::move(tool_config));
        sim.runtime.emplace(); // SIM profile: poisoned clocks, gas metering
        register_script_rng(*sim.runtime, static_cast<std::uint64_t>(seed));
        sim.scripts.emplace(*sim.runtime, *sim.toolchain, *sim.bus, &loader::resolve_key);
        if (auto error = sim.scripts->first_error())
            return refuse(std::move(*error));
        for (const loader::ScriptSeat& seat : spawned.scripts) {
            ecs::EntityRef host;
            for (const loader::MachineSeat& machine : spawned.machines)
                if (machine.id == seat.machine)
                    host = machine.host;
            if (auto error = sim.scripts->bind(
                    *sim.chart, seat.machine, seat.region, seat.state, host, seat.path))
                return refuse(std::move(*error));
            const std::size_t bound = sim.scripts->seat_count() - 1;
            Json entry = Json::object();
            entry.set("path", seat.path);
            entry.set("state",
                      std::string(seat.region.view()) + "/" + std::string(seat.state.view()));
            entry.set("module", sim.scripts->seat_module(bound));
            Json hooks = Json::array();
            for (const std::string_view hook : sim.scripts->seat_hooks(bound))
                hooks.push(std::string(hook));
            entry.set("hooks", std::move(hooks));
            scripts.push(std::move(entry));
        }
    }

    // Step. run_to_tick is a no-op past the target; tick(n) is exact. The
    // gc probes bracket EXACTLY the tick window: module build/bind churn
    // stays out of the ts_gc_churn evidence (RunProbes contract).
    const std::uint64_t gc_before = sim.runtime.has_value() ? sim.runtime->alloc_bytes() : 0;
    std::optional<base::Error> tick_error;
    if (has_to_tick)
        tick_error = sim.loop->run_to_tick(static_cast<std::uint64_t>(args.get_int("to-tick")));
    else if (has_ticks)
        tick_error = sim.loop->tick(static_cast<std::uint64_t>(args.get_int("ticks")));
    if (tick_error.has_value())
        return refuse(std::move(*tick_error));
    const std::uint64_t gc_after = sim.runtime.has_value() ? sim.runtime->alloc_bytes() : 0;

    // A script hook that faulted mid-run fails the run loudly (exit 1; the
    // error carries file/stack plus the sim tick it faulted on).
    if (sim.scripts.has_value())
        if (auto error = sim.scripts->first_error())
            return refuse(std::move(*error));

    if (auto error = sim.writer->close())
        return refuse(std::move(*error));

    const journal::TierConfig tiers = sim.writer->header().tiers;
    const std::string_view recorded_tier =
        tiers.trace ? "trace" : (tiers.snapshot ? "snapshot" : "flight");

    VerbOutcome out;

    // Post-close: the pack walks the recorded bundle + its live probes into
    // the named verdicts; any failed assertion fails the run (exit 1), with
    // every named value still reported in the envelope.
    if (sim.pack != nullptr) {
        RunAssertPack::RunProbes probes;
        probes.ticks = sim.loop->current_tick();
        probes.gc_alloc_before_ticks = gc_before;
        probes.gc_alloc_after_ticks = gc_after;
        probes.physics = sim.physics != nullptr ? &sim.physics->stats() : nullptr;
        RunAssertPack::Verdict verdict = sim.pack->evaluate(*sim.chart, bundle, probes);
        if (verdict.error.has_value())
            return refuse(std::move(*verdict.error));
        out.payload.set("assert_case", assert_case);
        out.payload.set("assertions", std::move(verdict.assertions));
        if (!verdict.exercised.is_null())
            out.payload.set("exercised", std::move(verdict.exercised));
        if (!verdict.failed.empty()) {
            out.exit = Exit::Failure;
            Error error{.code = "run.assert_failed",
                        .message = "assertion pack '" + assert_case + "' failed " +
                                   std::to_string(verdict.failed.size()) + " assertion(s)"};
            Json failed = Json::array();
            for (const std::string& name : verdict.failed)
                failed.push(name);
            error.details.set("failed", std::move(failed));
            out.error = std::move(error);
        }
    }

    out.payload.set("scene", scene_path);
    out.payload.set("scene_name", scene.name.view());
    out.payload.set("seed", seed);
    out.payload.set("ticks", static_cast<std::int64_t>(sim.loop->current_tick()));
    out.payload.set("recorded_tier", recorded_tier);
    out.payload.set("journal", bundle);
    out.payload.set("entities", spawned.stats.entities);
    out.payload.set("machines", spawned.stats.machines);
    out.payload.set("state_children", spawned.stats.state_children);
    out.payload.set("bodies", spawned.stats.bodies);
    out.payload.set("scripts", std::move(scripts));
    Json stats = Json::object();
    stats.set("transitions", static_cast<std::int64_t>(sim.chart->stats().transitions));
    stats.set("voided", static_cast<std::int64_t>(sim.chart->stats().voided));
    stats.set("sequence_triggers", static_cast<std::int64_t>(sim.chart->stats().sequence_triggers));
    stats.set("span_opens", static_cast<std::int64_t>(sim.chart->stats().span_opens));
    stats.set("bus_triggers", static_cast<std::int64_t>(sim.bus->stats().triggers));
    out.payload.set("stats", std::move(stats));
    out.human = std::string(scene.name.view()) + ": " + std::to_string(sim.loop->current_tick()) +
                " ticks, " + std::to_string(spawned.stats.entities) + " entities -> " + bundle;
    return out;
}

constexpr FlagSpec kFlags[] = {
    {.name = "ticks", .type = "int", .doc = "run exactly N fixed ticks"},
    {.name = "to-tick", .type = "int", .doc = "run until the sim tick reaches N"},
    {.name = "seed",
     .type = "int",
     .doc = "sim seed (journal identity + RNG streams)",
     .default_text = "0"},
    {.name = "record",
     .type = "string",
     .doc = "run.mrj bundle path (default: the .midday-cache/run/last.mrj scratch bundle)"},
    {.name = "cache-dir",
     .type = "string",
     .doc = "TS build cache directory (default: .midday-cache/ts)"},
    {.name = "assert",
     .type = "string",
     .doc = "drive + verify a registered assertion pack: case=<name> "
            "(available: appendix_a_golden, determinism_kata)"},
};

constexpr PositionalSpec kPositionals[] = {
    {.name = "scene", .type = "string", .doc = "the *.scene.yaml to load and run"},
};

} // namespace

const VerbSpec& run_spec() {
    static const VerbSpec spec{
        .name = "run",
        .summary = "load a scene and step the deterministic sim headless (FLIGHT-recorded)",
        .flags = kFlags,
        .positionals = kPositionals,
        .run = &run_verb,
    };
    return spec;
}

} // namespace midday::cli
