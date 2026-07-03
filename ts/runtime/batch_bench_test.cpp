// bindings.bench.* doctests — the budget harness contract, written against
// the budget-JSON shape FIRST (the jq predicates of the m0-batch-bindings
// exit tests, in C++ form): crossings <= 8 * pool_count and constant in N,
// zero steady-state GC bytes, >= 10x fewer crossings than naive mode, and
// dual-run determinism (same state hash, same budget). verify.sh runs the
// same predicates over the real 1k/10k/100k sweep via jq.

#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "ts/runtime/batch_bench.h"

#include <cstdint>
#include <string>

namespace {

using midday::base::Json;
using midday::script::BenchConfig;
using midday::script::BenchOutcome;
using midday::script::run_script_bench;

BenchConfig small_config(std::uint32_t entities, bool naive = false) {
    BenchConfig config;
    config.entities = entities;
    config.ticks = 6;
    config.warmup_ticks = 2;
    config.naive = naive;
    return config;
}

Json run_ok(const BenchConfig& config) {
    BenchOutcome outcome = run_script_bench(config);
    REQUIRE_MESSAGE(!outcome.error.has_value(),
                    (outcome.error ? outcome.error->message : std::string()));
    return std::move(outcome.budget);
}

std::int64_t field(const Json& budget, const char* key) {
    const Json* value = budget.find(key);
    REQUIRE_MESSAGE(value != nullptr, key);
    return value->as_int();
}

} // namespace

TEST_CASE("bindings.bench.budget_shape: the JSON the jq gates read, complete and in budget") {
    const Json budget = run_ok(small_config(64));
    CHECK(budget.find("mode")->as_string() == "batched");
    CHECK(field(budget, "entities") == 64);
    CHECK(field(budget, "ticks") == 6);
    CHECK(field(budget, "warmup_ticks") == 2);
    const std::int64_t pools = field(budget, "pool_count");
    CHECK(pools == 3);

    // THE exit-test predicates, verbatim in C++ form.
    CHECK(field(budget, "boundary_crossings_per_tick") <= 8 * pools);
    CHECK(field(budget, "gc_alloc_bytes_per_tick") == 0);
    CHECK(budget.find("crossings_constant")->as_bool());

    const Json* crossings = budget.find("crossings");
    REQUIRE(crossings != nullptr);
    // The fixture's decomposition: 8 refreshes (pos xyz + vel xyz + health
    // current/max), 7 commits (max is read_only), 1 tick entry, 0 hooks.
    CHECK(field(*crossings, "buffer_refreshes_per_tick") == 8);
    CHECK(field(*crossings, "buffer_commits_per_tick") == 7);
    CHECK(field(*crossings, "tick_entry_calls_per_tick") == 1);
    CHECK(field(*crossings, "host_hook_calls_per_tick") == 0);
    CHECK(field(budget, "boundary_crossings_per_tick") == 16);

    CHECK(field(budget, "gc_alloc_bytes_total") == 0);
    CHECK(budget.find("gas_used")->is_int());
    CHECK(budget.find("elapsed_ms")->is_int());
    CHECK(budget.find("state_hash")->as_string().size() == 16);
}

TEST_CASE("bindings.bench.constant_crossings: crossings independent of entity count") {
    const Json small = run_ok(small_config(32));
    const Json large = run_ok(small_config(1024));
    CHECK(field(small, "boundary_crossings_per_tick") ==
          field(large, "boundary_crossings_per_tick"));
    CHECK(field(large, "gc_alloc_bytes_per_tick") == 0);
}

TEST_CASE("bindings.bench.naive_ratio: batched crosses >= 10x less AND computes the same sim") {
    const Json batched = run_ok(small_config(64));
    const Json naive = run_ok(small_config(64, true));
    CHECK(naive.find("mode")->as_string() == "naive");
    // 18 crossings per entity per tick + count() + the tick entry.
    CHECK(field(naive, "boundary_crossings_per_tick") == 18 * 64 + 2);
    CHECK(field(naive, "boundary_crossings_per_tick") >=
          10 * field(batched, "boundary_crossings_per_tick"));
    // Parity pin: the chatty mode ran the SAME sim, bit for bit.
    CHECK(naive.find("state_hash")->as_string() == batched.find("state_hash")->as_string());
}

TEST_CASE("bindings.bench.determinism: two independent runs, identical budget and state") {
    Json first = run_ok(small_config(48));
    Json second = run_ok(small_config(48));
    CHECK(first.find("state_hash")->as_string() == second.find("state_hash")->as_string());
    first.set("elapsed_ms", std::int64_t{0}); // wall time is a report, not a contract
    second.set("elapsed_ms", std::int64_t{0});
    CHECK(first.dump() == second.dump());
}

TEST_CASE("bindings.bench.refusals: zero entities or ticks is a structured bad_request") {
    BenchConfig config;
    config.entities = 0;
    BenchOutcome outcome = run_script_bench(config);
    CHECK(midday::testkit::unwrap(outcome.error).code == "bindings.bad_request");
}
