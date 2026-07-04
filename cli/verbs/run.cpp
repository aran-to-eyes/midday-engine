// `midday run <scene> [--ticks N | --to-tick N] [--seed S] [--record path]`
// — the headless sim runner (m0-yaml-loader-run): authored YAML -> loader
// -> World/Hierarchy/Bus/TickLoop/Statechart (+ physics when the scene
// references it, + the TS toolchain when state scripts are referenced) ->
// fixed ticks -> a FLIGHT run.mrj bundle. Recording is ON BY CONSTRUCTION
// (Zenith D026): there is no run without a journal — omitting --record
// writes the scratch bundle .midday-cache/run/last.mrj (recreated per run;
// an EXPLICIT --record path never clobbers an existing bundle).
//
// Exit contract: 3 (validation) for every authored-text refusal (yaml.*,
// loader.*, script diagnostics — all with file:line), 1 for runtime/journal
// failures, 2 for usage. The envelope reports {ok, ticks, recorded_tier,
// journal, seed, scene, counts...} — the node's exit test asserts on it.

#include "cli/verb.h"
#include "core/bus/bus.h"
#include "core/ecs/world.h"
#include "core/hierarchy/hierarchy.h"
#include "core/journal/writer.h"
#include "core/loader/loader.h"
#include "core/physics/physics_server.h"
#include "core/reflect/builtin_events.h"
#include "core/reflect/registry.h"
#include "core/statechart/statechart.h"
#include "core/tick/tick_loop.h"
#include "ts/runtime/script_runtime.h"
#include "ts/toolchain/toolchain.h"

#include <cstdint>
#include <filesystem>
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
// and YAML layer emit carries file:line:col already.
bool is_validation(const Error& error) {
    return error.code.starts_with("loader.") || error.code.starts_with("yaml.") ||
           error.code.starts_with("script.");
}

VerbOutcome refuse(Error error) {
    VerbOutcome out;
    out.exit = is_validation(error) ? Exit::Validation : Exit::Failure;
    out.error = std::move(error);
    return out;
}

// The canonical sim composition (destruction order = reverse declaration:
// chart detaches its hooks first, then physics, loop, bus, writer).
struct RunSim {
    reflect::Registry registry;
    ecs::World world{registry};
    hierarchy::Hierarchy hierarchy{world};
    std::optional<journal::Writer> writer;
    std::optional<bus::Bus> bus;
    std::optional<tick::TickLoop> loop;
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

    // State scripts: build through the content-hash cache (typecheck + the
    // engine lint pack) and execute each module on the SIM runtime. Full
    // onX-hook parity is proven at m0-appendix-a-determinism; loading is
    // what this node's corpus needs.
    Json scripts = Json::array();
    if (!spawned.scripts.empty()) {
        script::ToolchainConfig tool_config;
        if (args.present("cache-dir"))
            tool_config.cache_dir = args.get_string("cache-dir");
        script::Toolchain toolchain(std::move(tool_config));
        script::ScriptRuntime runtime; // SIM profile: poisoned clocks, gas metering
        for (const loader::ScriptSeat& seat : spawned.scripts) {
            script::Toolchain::LoadOutcome module = toolchain.load_module(runtime, seat.path);
            if (module.error.has_value())
                return refuse(std::move(*module.error));
            Json entry = Json::object();
            entry.set("path", seat.path);
            entry.set("state",
                      std::string(seat.region.view()) + "/" + std::string(seat.state.view()));
            entry.set("module", module.resolved);
            scripts.push(std::move(entry));
        }
    }

    // Step. run_to_tick is a no-op past the target; tick(n) is exact.
    std::optional<base::Error> tick_error;
    if (has_to_tick)
        tick_error = sim.loop->run_to_tick(static_cast<std::uint64_t>(args.get_int("to-tick")));
    else if (has_ticks)
        tick_error = sim.loop->tick(static_cast<std::uint64_t>(args.get_int("ticks")));
    if (tick_error.has_value())
        return refuse(std::move(*tick_error));

    if (auto error = sim.writer->close())
        return refuse(std::move(*error));

    const journal::TierConfig tiers = sim.writer->header().tiers;
    const std::string_view recorded_tier =
        tiers.trace ? "trace" : (tiers.snapshot ? "snapshot" : "flight");

    VerbOutcome out;
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
