// ts/runtime/batch_bench.cpp — the budget harness (batch_bench.h). Builds a
// three-pool world, loads the TS fixture through the real toolchain (cache,
// typecheck, lint), drives the per-phase batch cycle, and measures the
// boundary: crossings and GC-allocated bytes per tick, plus a state hash
// proving two runs (and the naive comparison mode) computed the same sim.

#include "ts/runtime/batch_bench.h"

#include "core/ecs/world.h"
#include "core/reflect/registry.h"
#include "ts/runtime/batch_views.h"
#include "ts/runtime/script_runtime.h"
#include "ts/toolchain/toolchain.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace midday::script {
namespace {

using base::Error;
using base::Json;

struct BenchPosition {
    float x, y, z;
};

struct BenchVelocity {
    float x, y, z;
};

struct BenchHealth {
    float current;
    float max; // read_only: gathered into views, never scattered back
};

reflect::ClassDesc
bench_class(std::string_view name,
            std::initializer_list<std::pair<std::string_view, std::uint32_t>> fields) {
    reflect::ClassDesc desc;
    desc.name = base::Name(name);
    desc.doc = "script-bench fixture component (never registered globally)";
    for (const auto& [field, flags] : fields)
        desc.properties.push_back(
            reflect::PropertyDesc{.name = base::Name(field),
                                  .type = reflect::TypeDesc::scalar(reflect::TypeKind::kFloat),
                                  .default_value = Json(),
                                  .flags = flags,
                                  .doc = ""});
    return desc;
}

// Deterministic seed data: closed-form in the entity index, no RNG needed.
void spawn_fixture(ecs::World& world, std::uint32_t entities, std::vector<ecs::EntityRef>& refs) {
    refs.reserve(entities);
    for (std::uint32_t i = 0; i < entities; ++i) {
        const ecs::EntityRef ref = world.spawn();
        const auto f = static_cast<float>(i);
        world.emplace(ref, BenchPosition{f * 0.001F, f * -0.002F, f * 0.0005F});
        world.emplace(ref,
                      BenchVelocity{0.01F + static_cast<float>(i % 7) * 0.001F,
                                    -0.02F + static_cast<float>(i % 5) * 0.001F,
                                    0.03F - static_cast<float>(i % 3) * 0.001F});
        world.emplace(ref, BenchHealth{100.0F - static_cast<float>(i % 97), 100.0F});
        refs.push_back(ref);
    }
}

// XXH3-64 over the three dense value arrays, registration order — the
// "same sim" pin for dual runs and the batched-vs-naive parity check.
std::string state_hash(const ecs::World& world) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init): opaque XXH state
    XXH3_state_t state;
    XXH3_64bits_reset(&state);
    const auto mix = [&state](const auto& data) {
        XXH3_64bits_update(&state, data.data(), data.size() * sizeof(data[0]));
    };
    mix(world.pool<BenchPosition>().data());
    mix(world.pool<BenchVelocity>().data());
    mix(world.pool<BenchHealth>().data());
    char hex[17];
    std::snprintf(
        hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(XXH3_64bits_digest(&state)));
    return {hex};
}

// Per-field JSON-seam accessors — the classic chatty embedding this node
// exists to beat. Every call is one boundary crossing with JSON conversion
// on both sides; the budget report makes the cost visible (spec section 7).
void register_naive_hooks(ScriptRuntime& runtime,
                          ecs::World& world,
                          const std::vector<ecs::EntityRef>& refs) {
    const auto arg_error = [] {
        HostResult out;
        out.error = Error{.code = "bindings.bad_request",
                          .message = "naive hook: (index, component, field[, value])"};
        return out;
    };
    const auto field_slot = [&world, &refs](std::uint32_t index,
                                            const std::string& component,
                                            const std::string& field) -> float* {
        const ecs::EntityRef ref = refs[index];
        if (component == "position") {
            auto* value = world.try_get<BenchPosition>(ref);
            return field == "x" ? &value->x : (field == "y" ? &value->y : &value->z);
        }
        if (component == "velocity") {
            auto* value = world.try_get<BenchVelocity>(ref);
            return field == "x" ? &value->x : (field == "y" ? &value->y : &value->z);
        }
        auto* value = world.try_get<BenchHealth>(ref);
        return field == "current" ? &value->current : &value->max;
    };
    runtime.register_host_fn("__midday_naive_count", [&refs](const Json::Array&) {
        HostResult out;
        out.value = Json(static_cast<std::int64_t>(refs.size()));
        return out;
    });
    runtime.register_host_fn(
        "__midday_naive_get", [arg_error, field_slot](const Json::Array& args) {
            if (args.size() != 3 || !args[0].is_int() || !args[1].is_string() ||
                !args[2].is_string())
                return arg_error();
            HostResult out;
            out.value =
                Json(static_cast<double>(*field_slot(static_cast<std::uint32_t>(args[0].as_int()),
                                                     args[1].as_string(),
                                                     args[2].as_string())));
            return out;
        });
    runtime.register_host_fn(
        "__midday_naive_set", [arg_error, field_slot](const Json::Array& args) {
            if (args.size() != 4 || !args[0].is_int() || !args[1].is_string() ||
                !args[2].is_string() || !args[3].is_number())
                return arg_error();
            *field_slot(static_cast<std::uint32_t>(args[0].as_int()),
                        args[1].as_string(),
                        args[2].as_string()) =
                static_cast<float>(args[3].is_int() ? static_cast<double>(args[3].as_int())
                                                    : args[3].as_double());
            return HostResult{};
        });
}

struct TickSample {
    std::uint64_t crossings = 0;
    std::uint64_t alloc_bytes = 0;
    std::uint64_t hook_calls = 0;
    std::uint64_t tick_entries = 0;
    std::uint64_t refreshes = 0;
    std::uint64_t commits = 0;
};

} // namespace

BenchOutcome run_script_bench(const BenchConfig& config) {
    BenchOutcome out;
    out.budget = Json::object();
    if (config.entities == 0 || config.ticks == 0) {
        out.error = Error{.code = "bindings.bad_request",
                          .message = "script bench needs entities >= 1 and ticks >= 1"};
        return out;
    }

    reflect::Registry registry;
    ecs::World world(registry);
    world.register_component<BenchPosition>(
        bench_class("position", {{"x", 0}, {"y", 0}, {"z", 0}}));
    world.register_component<BenchVelocity>(
        bench_class("velocity", {{"x", 0}, {"y", 0}, {"z", 0}}));
    world.register_component<BenchHealth>(
        bench_class("health", {{"current", 0}, {"max", reflect::kPropertyReadOnly}}));
    std::vector<ecs::EntityRef> refs;
    spawn_fixture(world, config.entities, refs);

    ScriptRuntime runtime; // SIM profile: the fixture runs under sim rules
    BatchViews views(runtime, world, registry);
    views.expose<BenchPosition>("position")
        .field<&BenchPosition::x>("x")
        .field<&BenchPosition::y>("y")
        .field<&BenchPosition::z>("z");
    views.expose<BenchVelocity>("velocity")
        .field<&BenchVelocity::x>("x")
        .field<&BenchVelocity::y>("y")
        .field<&BenchVelocity::z>("z");
    views.expose<BenchHealth>("health")
        .field<&BenchHealth::current>("current")
        .field<&BenchHealth::max>("max");
    views.install();
    if (config.naive)
        register_naive_hooks(runtime, world, refs);

    ToolchainConfig tool_config;
    tool_config.cache_dir = config.cache_dir;
    Toolchain toolchain(std::move(tool_config));
    const std::string script = !config.script_path.empty() ? config.script_path
                               : config.naive ? std::string("testkit/fixtures/ts/naive_bench.ts")
                                              : std::string("testkit/fixtures/ts/batch_bench.ts");
    if (Toolchain::LoadOutcome loaded = toolchain.load_module(runtime, script); loaded.error) {
        out.error = std::move(loaded.error);
        return out;
    }

    const auto started = std::chrono::steady_clock::now();
    const std::uint64_t total = std::uint64_t{config.warmup_ticks} + config.ticks;
    std::vector<TickSample> samples;
    samples.reserve(config.ticks);
    TickSample final_split; // last measured tick's per-counter deltas
    BatchStats last_stats;
    std::uint64_t last_hooks = runtime.host_calls();
    std::uint64_t last_alloc = runtime.alloc_bytes();
    for (std::uint64_t tick = 0; tick < total; ++tick) {
        std::optional<Error> error;
        if (config.naive) {
            // Naive ticks enter through the JSON seam on purpose — that IS
            // the mode under measurement.
            EvalResult result =
                runtime.call_json("__midday_batch_tick", Json(static_cast<std::int64_t>(tick)));
            error = std::move(result.error);
        } else {
            error = views.refresh(tick);
            if (!error)
                error = views.call_tick(tick);
            if (!error)
                error = views.commit();
        }
        if (error) {
            annotate_sim_context(*error, tick, "script-bench");
            out.error = std::move(error);
            return out;
        }
        const BatchStats& stats = views.stats();
        const std::uint64_t hooks = runtime.host_calls();
        const std::uint64_t alloc = runtime.alloc_bytes();
        if (tick >= config.warmup_ticks) {
            const std::uint64_t tick_entry = config.naive ? 1 : 0; // call_json entries
            final_split = TickSample{
                .crossings = 0,
                .alloc_bytes = alloc - last_alloc,
                .hook_calls = hooks - last_hooks,
                .tick_entries = tick_entry + (stats.tick_calls - last_stats.tick_calls),
                .refreshes = (stats.buffer_refreshes - last_stats.buffer_refreshes) +
                             (stats.view_rebuilds - last_stats.view_rebuilds),
                .commits = stats.buffer_commits - last_stats.buffer_commits,
            };
            final_split.crossings = final_split.hook_calls + final_split.tick_entries +
                                    final_split.refreshes + final_split.commits;
            samples.push_back(final_split);
        }
        last_stats = stats;
        last_hooks = hooks;
        last_alloc = alloc;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();

    std::uint64_t max_crossings = 0;
    std::uint64_t max_alloc = 0;
    std::uint64_t total_alloc = 0;
    bool constant = true;
    for (const TickSample& sample : samples) {
        constant = constant && sample.crossings == samples.front().crossings;
        max_crossings = std::max(max_crossings, sample.crossings);
        max_alloc = std::max(max_alloc, sample.alloc_bytes);
        total_alloc += sample.alloc_bytes;
    }

    Json& budget = out.budget;
    budget.set("mode", config.naive ? "naive" : "batched");
    budget.set("entities", static_cast<std::int64_t>(config.entities));
    budget.set("ticks", static_cast<std::int64_t>(config.ticks));
    budget.set("warmup_ticks", static_cast<std::int64_t>(config.warmup_ticks));
    budget.set("pool_count", static_cast<std::int64_t>(views.exposed_components()));
    budget.set("boundary_crossings_per_tick", static_cast<std::int64_t>(max_crossings));
    budget.set("crossings_constant", constant);
    Json crossings = Json::object(); // the last measured tick, decomposed
    crossings.set("host_hook_calls_per_tick", static_cast<std::int64_t>(final_split.hook_calls));
    crossings.set("tick_entry_calls_per_tick", static_cast<std::int64_t>(final_split.tick_entries));
    crossings.set("buffer_refreshes_per_tick", static_cast<std::int64_t>(final_split.refreshes));
    crossings.set("buffer_commits_per_tick", static_cast<std::int64_t>(final_split.commits));
    budget.set("crossings", std::move(crossings));
    budget.set("gc_alloc_bytes_per_tick", static_cast<std::int64_t>(max_alloc));
    budget.set("gc_alloc_bytes_total", static_cast<std::int64_t>(total_alloc));
    budget.set("gas_used", static_cast<std::int64_t>(runtime.gas_used()));
    budget.set("state_hash", state_hash(world));
    budget.set("elapsed_ms", static_cast<std::int64_t>(elapsed));
    return out;
}

} // namespace midday::script
