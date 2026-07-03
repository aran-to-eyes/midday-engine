// ts/runtime/script_runtime.h — the embedded QuickJS script runtime (spec
// section 7, m0-quickjs-ts-toolchain). One class owns the JSRuntime/JSContext
// pair, its resource limits, the module loader, and the host-hook surface.
// QuickJS types never leak through this header: callers speak base::Json and
// base::Error only.
//
// Two profiles, one switch (RuntimeConfig::deterministic):
//   * SIM (default): scripts run inside the deterministic simulation — no
//     wall clock, no unseeded randomness. Date, performance, and Math.random
//     are poisoned by a JS prelude at context creation (they throw
//     script.nondeterminism); setTimeout/setInterval never exist because
//     quickjs-libc is not vendored. Poisoning happens at the JS level, never
//     by patching vendored source (third_party/CMakeLists.txt audit note).
//   * TOOL (deterministic = false): the TS toolchain's own runtime — the
//     vendored TypeScript compiler may read the clock for its internals; its
//     OUTPUT stays byte-deterministic (proven by the script.cache doctests).
//
// Interrupt budget ("gas"): QuickJS polls its interrupt handler once every
// 10000 VM poll points (function calls + loop back-edges — see
// JS_INTERRUPT_COUNTER_INIT in third_party/quickjs/quickjs.c). That cadence
// is a pure function of the executed program, so a gas limit expressed in
// handler invocations is DETERMINISTIC: the same script exceeds the same
// budget at the same VM step on every platform. Never wall-clock-based.
// Bypass policy: none — there is no API to re-enable the poisoned globals.

#pragma once

#include "core/base/error.h"
#include "core/base/json.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct JSContext; // quickjs.h's own tag — the batch seam below hands it out

namespace midday::script {

class ScriptRuntime;

namespace detail {
// Internal seam for ts/runtime/batch_views.cpp ONLY: the batch bindings are
// the one consumer allowed to speak QuickJS directly (typed arrays cannot
// cross the JSON hook seam). Everything else keeps talking base::Json.
JSContext* runtime_context(ScriptRuntime& runtime);
base::Error runtime_take_exception(ScriptRuntime& runtime);
} // namespace detail

struct RuntimeConfig {
    // Hard heap cap enforced by QuickJS's allocator; exceeding it surfaces
    // as a structured script.out_of_memory error, never a crash.
    std::size_t memory_limit_bytes = 256u << 20;
    // Interrupt-budget cap in handler invocations (~10k VM poll points each,
    // see the header comment). 0 = unlimited. Exceeding it aborts the script
    // with script.interrupted.
    std::uint64_t gas_limit = 0;
    // QuickJS C-stack budget (its recursion guard measures REAL stack bytes,
    // so achievable JS depth varies with build flags — recursion depth is
    // deliberately NOT part of the determinism contract). The toolchain
    // raises this for the compiler; the midday binary reserves matching OS
    // stack on Windows (cli/CMakeLists.txt).
    std::size_t stack_size_bytes = 1u << 20;
    // SIM profile: poison Date / performance / Math.random (see above).
    bool deterministic = true;
};

// What the module loader serves. `resolved` is the canonical module name the
// runtime registers (it becomes the file in stack traces); `js_source` is
// the compiled JavaScript. Scripts can only import what a resolver returns —
// there is no ambient filesystem access.
struct ModuleSource {
    std::string resolved;
    std::string js_source;
};

// specifier: the import string as written; referrer: the resolved name of
// the importing module ("" for a top-level load). nullopt = unresolvable,
// surfaced as a structured script.module_not_found error.
using ModuleResolver = std::function<std::optional<ModuleSource>(std::string_view specifier,
                                                                 std::string_view referrer)>;

// Host hook: a native function exposed as globalThis.<name>. Arguments and
// result cross as JSON values (the batch bindings of m0-batch-bindings are
// the high-bandwidth path; hooks are the minimal bootstrap seam). Returning
// an Error in HostResult throws a structured exception into the script.
struct HostResult {
    base::Json value;
    std::optional<base::Error> error;
};

using HostFn = std::function<HostResult(const base::Json::Array& args)>;

// JSON-valued evaluation result. On failure `error` carries the structured
// shape: code script.* with details {file, line, col, stack} — plus slots
// {tick, replay_bookmark} that the SIM CALLER fills via annotate_sim_context
// (the runtime cannot know sim time; the tick loop does).
struct EvalResult {
    base::Json value;
    std::optional<base::Error> error;
};

class ScriptRuntime {
public:
    explicit ScriptRuntime(RuntimeConfig config = {});
    ~ScriptRuntime();
    ScriptRuntime(const ScriptRuntime&) = delete;
    ScriptRuntime(ScriptRuntime&&) = delete;
    ScriptRuntime& operator=(const ScriptRuntime&) = delete;
    ScriptRuntime& operator=(ScriptRuntime&&) = delete;

    // Scripts may only import what this resolver serves.
    void set_module_resolver(ModuleResolver resolver);

    // Expose a native function as globalThis.<name>. Registration is
    // idempotent per name (last registration wins).
    void register_host_fn(std::string name, HostFn fn);

    // Evaluate a classic (non-module) script in the global scope. The
    // toolchain bootstraps the vendored typescript.js through this.
    std::optional<base::Error> eval_global(std::string_view source, std::string_view filename);

    // Call globalThis.<fn>(<one JSON argument>) and JSON-convert the result.
    // The C++ <-> driver traffic of the TS toolchain runs through this seam.
    EvalResult call_json(std::string_view fn, const base::Json& argument);

    // Resolve `specifier` through the module resolver, evaluate the module
    // graph (imports recurse through the resolver), and run it to completion
    // (pending microtasks included). Returns the resolved module name.
    struct LoadedModule {
        std::string resolved;
        std::optional<base::Error> error;
    };

    LoadedModule load_module(std::string_view specifier);

    // Interrupt-handler invocations consumed since construction.
    [[nodiscard]] std::uint64_t gas_used() const;

    // Cumulative JS-heap bytes ALLOCATED since construction (counting
    // allocator; frees never decrement — this measures churn, not
    // residency). The batch bindings' GC budget: a steady-state tick that
    // allocates zero bytes cannot trigger GC pauses, ever.
    [[nodiscard]] std::uint64_t alloc_bytes() const;

    // Cumulative host-hook invocations (the JSON seam) — one boundary
    // crossing each. The batch bindings' crossing budget counts these
    // alongside its own buffer publishes.
    [[nodiscard]] std::uint64_t host_calls() const;

private:
    friend struct RuntimeBridge; // QuickJS callback trampolines (script_runtime.cpp)
    friend JSContext* detail::runtime_context(ScriptRuntime&);
    friend base::Error detail::runtime_take_exception(ScriptRuntime&);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Fill the {tick, replay_bookmark} slots of a script error. The runtime
// leaves them absent; the sim caller (tick loop / test harness) owns them.
void annotate_sim_context(base::Error& error, std::uint64_t tick, std::string_view replay_bookmark);

} // namespace midday::script
