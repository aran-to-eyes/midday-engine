// ts/runtime/state_script.cpp — see state_script.h for the seam contract.

#include "ts/runtime/state_script.h"

#include "core/base/json.h"

#include <string>
#include <utility>

namespace midday::script {
namespace {

// The JS half of the generated hook seam (api/bindings_spec.json
// "state_script_hooks"): a seat registry keyed by integer, instantiation at
// registration (boot-deterministic), hook introspection so absent hooks
// never cross the boundary, and the invoke dispatcher. First-party fixed
// source — like the SIM prelude, it cannot throw.
constexpr std::string_view kStateScriptPrelude = R"js("use strict";
globalThis.__midday_state_seats = new Map();
globalThis.__midday_register_state_script = function (seat, cls) {
    if (typeof cls !== "function")
        throw new TypeError("state script default export must be a class");
    __midday_state_seats.set(seat, new cls());
};
globalThis.__midday_state_hooks_of = function (seat) {
    const s = __midday_state_seats.get(seat);
    return {
        onEnter: typeof s.onEnter === "function",
        onExit: typeof s.onExit === "function",
        onUpdate: typeof s.onUpdate === "function",
        onFixedUpdate: typeof s.onFixedUpdate === "function",
    };
};
globalThis.__midday_invoke_state_hook = function (call) {
    const s = __midday_state_seats.get(call.seat);
    s[call.hook](call.arg);
    return null;
};
)js";

} // namespace

StateScriptHost::StateScriptHost(ScriptRuntime& runtime,
                                 Toolchain& toolchain,
                                 bus::Bus& bus,
                                 KeyResolver resolve_key)
    : runtime_(&runtime), toolchain_(&toolchain), bus_(&bus), resolve_key_(resolve_key) {
    // Fixed first-party source: an error here is a build defect, surfaced
    // through first_error_ so the run host still fails loudly.
    first_error_ = runtime_->eval_global(kStateScriptPrelude, "<midday:state-script-prelude>");
    runtime_->register_host_fn(std::string(kEmitFn),
                               [this](const base::Json::Array& args) { return emit(args); });
}

std::optional<base::Error> StateScriptHost::bind(statechart::Statechart& chart,
                                                 statechart::MachineId machine,
                                                 base::Name region,
                                                 base::Name state,
                                                 ecs::EntityRef host,
                                                 const std::string& path) {
    Toolchain::LoadOutcome loaded = toolchain_->load_module(*runtime_, path);
    if (loaded.error.has_value())
        return std::move(loaded.error);

    // The registration shim: import the module BY ITS CANONICAL NAME (an
    // already-registered name imports as itself — ScriptRuntime contract)
    // and seat its default export. Generated per seat, never authored.
    const auto seat_id = static_cast<std::int64_t>(seats_.size());
    const std::string shim = "import M from " + base::Json(loaded.resolved).dump() + ";\n" +
                             std::string(kRegisterFn) + "(" + std::to_string(seat_id) + ", M);\n";
    const std::string shim_name = "<midday:state-script-seat:" + std::to_string(seat_id) + ">";
    ScriptRuntime::LoadedModule bound = runtime_->load_module_source(shim_name, shim);
    if (bound.error.has_value()) {
        bound.error->details.set("script", path);
        return std::move(bound.error);
    }

    EvalResult flags = runtime_->call_json(kIntrospectFn, base::Json(seat_id));
    if (flags.error.has_value())
        return std::move(flags.error);

    Seat seat;
    seat.machine = machine;
    seat.region = region;
    seat.state = state;
    seat.host = host;
    seat.resolved = std::move(loaded.resolved);
    for (std::size_t i = 0; i < 4; ++i) {
        const base::Json* flag = flags.value.find(kHookNames[i]);
        seat.has[i] = flag != nullptr && flag->is_bool() && flag->as_bool();
    }
    if (auto error = chart.set_state_hooks(machine, region, state, *this))
        return error;
    seats_.push_back(std::move(seat));
    return std::nullopt;
}

const StateScriptHost::Seat*
StateScriptHost::find_seat(const statechart::StateHookContext& ctx) const {
    for (const Seat& seat : seats_)
        if (seat.machine == ctx.machine && seat.region == ctx.region && seat.state == ctx.state)
            return &seat;
    return nullptr;
}

void StateScriptHost::invoke(const statechart::StateHookContext& ctx, Hook hook) {
    const Seat* seat = find_seat(ctx);
    const auto index = static_cast<std::size_t>(hook);
    if (seat == nullptr || !seat->has[index])
        return;

    base::Json call = base::Json::object();
    call.set("seat", static_cast<std::int64_t>(seat - seats_.data()));
    call.set("hook", kHookNames[index]);
    if (hook == Hook::kEnter || hook == Hook::kExit)
        call.set("arg", std::string(ctx.peer.view()));
    else
        call.set("arg", ctx.dt);

    emit_stack_.push_back(EmitFrame{seat->host, ctx.record_id, ctx.tick});
    EvalResult result = runtime_->call_json(kInvokeFn, call);
    emit_stack_.pop_back();

    if (result.error.has_value() && !first_error_.has_value()) {
        annotate_sim_context(*result.error, ctx.tick, "");
        result.error->details.set("script", seat->resolved);
        result.error->details.set("hook", kHookNames[index]);
        first_error_ = std::move(result.error);
    }
}

HostResult StateScriptHost::emit(const base::Json::Array& args) {
    if (emit_stack_.empty())
        return {base::Json(),
                base::Error{.code = "script.emit_outside_hook",
                            .message =
                                std::string(kEmitFn) + " may only run inside a state-script hook",
                            .details = base::Json::object()}};
    if (args.size() != 3 || !args[0].is_string() || !args[2].is_string())
        return {base::Json(),
                base::Error{.code = "script.emit_args",
                            .message = std::string(kEmitFn) +
                                       " expects (event: string, payload, key: string)",
                            .details = base::Json::object()}};

    const EmitFrame& frame = emit_stack_.back();
    const bus::EventKey key = resolve_key_(args[2].as_string(), frame.host);
    bus::TriggerResult triggered =
        bus_->trigger(key, base::Name(args[0].as_string()), args[1], frame.record_id);
    if (triggered.error.has_value())
        return {base::Json(), std::move(triggered.error)};
    return {base::Json(static_cast<std::int64_t>(triggered.record_id)), std::nullopt};
}

void StateScriptHost::on_enter(statechart::Statechart& /*chart*/,
                               const statechart::StateHookContext& ctx) {
    invoke(ctx, Hook::kEnter);
}

void StateScriptHost::on_exit(statechart::Statechart& /*chart*/,
                              const statechart::StateHookContext& ctx) {
    invoke(ctx, Hook::kExit);
}

void StateScriptHost::on_update(statechart::Statechart& /*chart*/,
                                const statechart::StateHookContext& ctx) {
    invoke(ctx, Hook::kUpdate);
}

void StateScriptHost::on_fixed_update(statechart::Statechart& /*chart*/,
                                      const statechart::StateHookContext& ctx) {
    invoke(ctx, Hook::kFixedUpdate);
}

} // namespace midday::script
