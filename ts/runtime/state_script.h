// ts/runtime/state_script.h — the TS state-script binding
// (m0-appendix-a-determinism): authored `script:` modules drive the SAME
// A.2.1 hook semantics the C++ fixtures pinned at m0-statechart-core. One
// StateScriptHost implements statechart::StateHooks for every seated script:
// it builds modules through the toolchain cache, instantiates each module's
// default-export class on the SIM runtime, and dispatches
// onEnter/onExit/onUpdate/onFixedUpdate through the generated hook seam
// (api/bindings_spec.json "state_script_hooks"; the golden.ts_hook_parity
// drift test pins the constants below against the committed artifact).
//
// The seam, end to end:
//   * bind(): toolchain.load_module (typecheck + lint + cache) evaluates the
//     module; a generated one-line shim module then imports it BY ITS
//     CANONICAL NAME and registers the default export with the prelude
//     (`new cls()` runs at bind — boot-deterministic). The prelude reports
//     which of the four hooks the instance actually has, so absent hooks
//     never cross the boundary (no gas, no crossings — D-BUILD-071 spirit).
//   * hook dispatch: one call_json per PRESENT hook — payload {seat, hook,
//     arg} where arg is the peer state name (enter/exit) or dt (update
//     flavors), exactly the StateHookContext the C++ seat receives (A.2.1).
//   * __midday_emit(event, payload, key): the state-script emit seat. Keys
//     use the spec 4.2 symbolic vocabulary resolved by the INJECTED resolver
//     (the loader's resolve_key in production — ts/ never depends on
//     core/loader); the trigger's cause id is THE CURRENT HOOK'S journal
//     record (StateHookContext::record_id), so cause chains — A.3's
//     "transition -> Dead.onEnter -> boss.died" — reconstruct mechanically.
//
// Script exceptions inside a hook cannot unwind the transition machinery
// (hooks return void by contract); the first structured error is captured —
// annotated with the sim tick — and surfaced via first_error() for the run
// host to fail the run loudly.

#pragma once

#include "core/base/error.h"
#include "core/base/name.h"
#include "core/bus/bus.h"
#include "core/ecs/entity.h"
#include "core/statechart/statechart.h"
#include "ts/runtime/script_runtime.h"
#include "ts/toolchain/toolchain.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace midday::script {

class ComponentHost;

// Symbolic-key resolution (spec 4.2: self | root | global | <group>). The
// production resolver is loader::resolve_key; injecting the function keeps
// the ts/ tier free of loader dependencies.
using KeyResolver = bus::EventKey (*)(std::string_view spelling, ecs::EntityRef host);

class StateScriptHost final : public statechart::StateHooks {
public:
    // The generated seam contract — api/bindings_spec.json
    // "state_script_hooks" carries these exact spellings (drift-gated by
    // golden.ts_hook_parity.seam_matches_bindings_spec).
    static constexpr std::int64_t kEnvelopeVersion = 1;
    static constexpr std::string_view kRegisterFn = "__midday_register_state_script";
    static constexpr std::string_view kIntrospectFn = "__midday_state_hooks_of";
    static constexpr std::string_view kInvokeFn = "__midday_invoke_state_hook";
    static constexpr std::string_view kEmitFn = "__midday_emit";
    static constexpr std::string_view kHookNames[4] = {
        "onEnter", "onExit", "onUpdate", "onFixedUpdate"};

    // All collaborators must outlive the host; the host must outlive every
    // machine it seats (destroy order: Statechart first, then this, per the
    // StateHooks lifetime contract).
    StateScriptHost(ScriptRuntime& runtime,
                    Toolchain& toolchain,
                    bus::Bus& bus,
                    KeyResolver resolve_key);

    StateScriptHost(const StateScriptHost&) = delete;
    StateScriptHost& operator=(const StateScriptHost&) = delete;
    StateScriptHost(StateScriptHost&&) = delete;
    StateScriptHost& operator=(StateScriptHost&&) = delete;
    ~StateScriptHost() = default;

    // Build + evaluate + instantiate + seat `path` on (machine, region,
    // state) of `chart`. `host` is the machine's host entity — the `self`/
    // `root` channel for this seat's emits. Errors are structured (script.*
    // diagnostics carry file:line; statechart.* name the bad seat).
    std::optional<base::Error> bind(statechart::Statechart& chart,
                                    statechart::MachineId machine,
                                    base::Name region,
                                    base::Name state,
                                    ecs::EntityRef host,
                                    const std::string& path);

    // The first hook-invocation error, sim-tick annotated (empty = clean).
    [[nodiscard]] const std::optional<base::Error>& first_error() const { return first_error_; }

    // M2 0B: bridge hook causality into the ts/lib emit path — while a hook
    // runs, its journal record id rides `primitives`' cause-frame stack
    // (ComponentHost::push_cause), so a state script calling the LIBRARY
    // surface (events.trigger / this.emit — __midday_trigger_*) cites the
    // hook record exactly like __midday_emit always has. Production wires
    // this whenever both hosts exist (run.cpp); unset keeps the pre-0B
    // cause (0 — root) for library emits.
    void set_cause_bridge(ComponentHost& primitives) { primitives_ = &primitives; }

    [[nodiscard]] std::size_t seat_count() const { return seats_.size(); }

    // Seat introspection (bind order): canonical module name + the hook
    // names the instance actually has (the run envelope reports both).
    [[nodiscard]] const std::string& seat_module(std::size_t index) const {
        return seats_[index].resolved;
    }

    [[nodiscard]] std::vector<std::string_view> seat_hooks(std::size_t index) const {
        std::vector<std::string_view> present;
        for (std::size_t i = 0; i < 4; ++i)
            if (seats_[index].has[i])
                present.push_back(kHookNames[i]);
        return present;
    }

    // statechart::StateHooks — dispatch into the seated JS instance.
    void on_enter(statechart::Statechart& chart, const statechart::StateHookContext& ctx) override;
    void on_exit(statechart::Statechart& chart, const statechart::StateHookContext& ctx) override;
    void on_update(statechart::Statechart& chart, const statechart::StateHookContext& ctx) override;
    void on_fixed_update(statechart::Statechart& chart,
                         const statechart::StateHookContext& ctx) override;

private:
    enum class Hook : std::uint8_t { kEnter = 0, kExit = 1, kUpdate = 2, kFixedUpdate = 3 };

    struct Seat {
        statechart::MachineId machine = statechart::kInvalidMachine;
        base::Name region;
        base::Name state;
        ecs::EntityRef host;
        bool has[4] = {false, false, false, false};
        std::string resolved; // canonical module name (diagnostics)
    };

    // The emit context while a hook runs: cause id + channel owner.
    struct EmitFrame {
        ecs::EntityRef host;
        std::uint64_t record_id = 0;
        std::uint64_t tick = 0;
    };

    [[nodiscard]] const Seat* find_seat(const statechart::StateHookContext& ctx) const;
    void invoke(const statechart::StateHookContext& ctx, Hook hook);
    HostResult emit(const base::Json::Array& args);

    ScriptRuntime* runtime_;
    Toolchain* toolchain_;
    bus::Bus* bus_;
    KeyResolver resolve_key_;
    ComponentHost* primitives_ = nullptr; // the cause bridge (set_cause_bridge)
    std::vector<Seat> seats_;
    std::vector<EmitFrame> emit_stack_;
    std::optional<base::Error> first_error_;
};

} // namespace midday::script
