// ts/runtime/batch_bench.h — the batch-binding budget harness behind
// `midday script bench` (m0-batch-bindings exit tests). Spawns N entities
// with three component pools (position/velocity/health), runs a TS fixture
// updating them for `ticks` measured ticks, and emits the budget JSON the
// sweep gates assert with jq (D-BUILD-071):
//
//   { mode, entities, ticks, warmup_ticks, pool_count,
//     boundary_crossings_per_tick,            // max over measured ticks
//     crossings_constant,                     // every measured tick equal
//     crossings: { host_hook_calls_per_tick, tick_entry_calls_per_tick,
//                  buffer_refreshes_per_tick, buffer_commits_per_tick },
//     gc_alloc_bytes_per_tick,                // max over measured ticks
//     gc_alloc_bytes_total, gas_used, state_hash, elapsed_ms }
//
// Budget contracts (verify.sh sweeps 1k/10k/100k over 60 ticks):
//   boundary_crossings_per_tick <= 8 * pool_count   — and constant in N
//   gc_alloc_bytes_per_tick == 0                    — pooled math, batched
//   naive mode (per-field JSON hooks) >= 10x more crossings
//
// Warmup ticks run the same cycle before measurement so one-time JS warmup
// (shapes, pool growth, buffer builds) never pollutes the steady-state
// numbers; state_hash covers warmup+measured ticks, so two runs of the same
// config must hash identically (bindings.bench determinism doctest).

#pragma once

#include "core/base/error.h"
#include "core/base/json.h"

#include <cstdint>
#include <optional>
#include <string>

namespace midday::script {

struct BenchConfig {
    std::uint32_t entities = 1000;
    std::uint32_t ticks = 60;
    std::uint32_t warmup_ticks = 5;
    bool naive = false;      // per-field host-hook accessors (the comparison)
    std::string script_path; // empty = the committed fixture for the mode
    std::string cache_dir = ".midday-cache/ts";
};

struct BenchOutcome {
    base::Json budget; // the shape above (empty object on error)
    std::optional<base::Error> error;
};

BenchOutcome run_script_bench(const BenchConfig& config);

} // namespace midday::script
