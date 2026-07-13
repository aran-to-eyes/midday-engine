// core/statechart/statechart.cpp — construction/destruction, component
// registration, phase dispatch, environment binding, introspection, and
// state.finished emission. The A.2 algorithm lives in transitions.cpp, the
// phase bodies in watchers.cpp / hooks.cpp, machine building in
// instantiate.cpp.

#include "core/statechart/statechart.h"

#include "core/base/error.h"
#include "core/base/json.h"
#include "core/bus/entity_listener.h"
#include "core/ecs/world.h"
#include "core/hierarchy/hierarchy.h"
#include "core/journal/writer.h"
#include "core/reflect/registry.h"
#include "core/statechart/instance.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

namespace midday::statechart {

namespace detail {

// Invariant breaks (unbalanced activation scopes, impossible hook wiring)
// abort loudly — the reflect fatal precedent: never throw, never corrupt.
[[noreturn]] void fatal(const char* what, const base::Error& error) {
    std::fprintf(stderr,
                 "midday: fatal: statechart: %s: %s: %s\n",
                 what,
                 error.code.c_str(),
                 error.message.c_str());
    std::abort();
}

void fatal_if(const char* what, const std::optional<base::Error>& error) {
    if (error.has_value())
        fatal(what, *error);
}

} // namespace detail

namespace {

reflect::ClassDesc component_class(const char* name, const char* doc) {
    reflect::ClassDesc cls;
    cls.name = base::Name(name);
    cls.doc = doc;
    return cls;
}

} // namespace

// ---- small shared helpers (instance.h) -------------------------------------

expr::Value zero_value(expr::ValueType type) {
    switch (type) {
    case expr::ValueType::kBool:
        return expr::Value::of_bool(false);
    case expr::ValueType::kInt:
        return expr::Value::of_int(0);
    case expr::ValueType::kFloat:
        return expr::Value::of_float(0.0F);
    case expr::ValueType::kString: {
        constexpr std::string_view kEmpty = ""; // literal storage, process lifetime
        return expr::Value::of_string(kEmpty);
    }
    case expr::ValueType::kName:
        return expr::Value::of_name(base::Name());
    case expr::ValueType::kVec2:
        return expr::Value::of_vec2({});
    case expr::ValueType::kVec3:
        return expr::Value::of_vec3({});
    case expr::ValueType::kVec4:
        return expr::Value::of_vec4({});
    case expr::ValueType::kQuat:
        return expr::Value::of_quat({});
    }
    return expr::Value::of_bool(false); // unreachable
}

std::string entity_form(ecs::EntityRef ref) {
    return "entity:" + std::to_string(ref.index) + "#" + std::to_string(ref.generation);
}

// ---- hook interface defaults ------------------------------------------------

void StateHooks::on_enter(Statechart&, const StateHookContext&) {}

void StateHooks::on_exit(Statechart&, const StateHookContext&) {}

void StateHooks::on_update(Statechart&, const StateHookContext&) {}

void StateHooks::on_fixed_update(Statechart&, const StateHookContext&) {}

// ---- the ECS delivery thunk target ------------------------------------------

void MachineRoot::on_event(bus::Bus&, const bus::EventView& event) {
    if (system != nullptr)
        system->on_machine_event(machine, event);
}

// ---- Statechart --------------------------------------------------------------

Statechart::Statechart(ecs::World& world,
                       hierarchy::Hierarchy& hierarchy,
                       bus::Bus& bus,
                       journal::Writer& journal,
                       tick::TickLoop& loop)
    : world_(&world), hierarchy_(&hierarchy), bus_(&bus), journal_(&journal), loop_(&loop) {
    world_->register_component<MachineRoot>(
        component_class("StatechartMachine",
                        "Machine-instance marker on the machine root entity: the bus "
                        "delivery target (written only by statechart::Statechart)."));
    world_->register_component<StateNode>(
        component_class("StatechartState",
                        "State identity on every state entity: machine/region/state names "
                        "for introspection (written only by statechart::Statechart)."));
    detail::fatal_if("add watchers hook", loop_->add_hook(tick::Phase::kWatchers, *this));
    detail::fatal_if("add sequences hook", loop_->add_hook(tick::Phase::kSequences, *this));
    detail::fatal_if("add update hook", loop_->add_hook(tick::Phase::kUpdate, *this));
    // Frames for every legal nesting level (bus cap + phase-level frame 0):
    // sized ONCE so nested dispatch never reallocates under an outer frame.
    scratch_.resize(bus::Bus::kMaxCascadeDepth + 2);
}

Statechart::~Statechart() {
    // Boot-locked hook surface: destruction mid-tick is a contract violation
    // and surfaces here as a loud abort rather than a dangling hook pointer.
    detail::fatal_if("remove watchers hook", loop_->remove_hook(tick::Phase::kWatchers, *this));
    detail::fatal_if("remove sequences hook", loop_->remove_hook(tick::Phase::kSequences, *this));
    detail::fatal_if("remove update hook", loop_->remove_hook(tick::Phase::kUpdate, *this));
    for (const std::unique_ptr<MachineInstance>& instance : machines_) {
        if (instance->retired)
            continue;
        for (const bus::EventKey& key : instance->keys)
            (void)bus::unsubscribe_component<MachineRoot>(*bus_, key, instance->root);
    }
}

void Statechart::on_phase(tick::TickLoop&, const tick::PhaseContext& context) {
    if (context.phase == tick::Phase::kWatchers)
        run_watchers(context);
    else if (context.phase == tick::Phase::kSequences)
        run_sequences(context);
    else if (context.phase == tick::Phase::kUpdate)
        run_update_hooks(true, context.dt, context.phase_record_id);
}

MachineInstance* Statechart::find_machine(MachineId machine) const {
    if (machine >= machines_.size())
        return nullptr;
    return machines_[machine].get();
}

bool Statechart::machine_live(MachineInstance& instance) {
    if (instance.retired)
        return false;
    if (!world_->alive(instance.root)) {
        instance.retired = true; // stale subscriptions auto-drop at the bus
        return false;
    }
    return true;
}

std::optional<base::Error> Statechart::set_state_hooks(MachineId machine,
                                                       base::Name region,
                                                       base::Name state,
                                                       StateHooks& hooks) {
    MachineInstance* instance = find_machine(machine);
    if (instance == nullptr)
        return base::Error{"statechart.unknown_machine", "no machine with this id"};
    for (const RtRegion& rt_region : instance->regions) {
        if (rt_region.name != region)
            continue;
        for (std::uint32_t s = rt_region.first_state;
             s < rt_region.first_state + rt_region.state_count;
             ++s) {
            if (instance->states[s].name == state) {
                instance->states[s].hooks = &hooks;
                return std::nullopt;
            }
        }
        return base::Error{"statechart.unknown_state", "no such state in the region"};
    }
    return base::Error{"statechart.unknown_region", "no such region in the machine"};
}

std::optional<base::Error> Statechart::add_component_hooks(MachineId machine,
                                                           base::Name region,
                                                           base::Name state,
                                                           base::Name component,
                                                           ComponentHooks& hooks) {
    MachineInstance* instance = find_machine(machine);
    if (instance == nullptr)
        return base::Error{"statechart.unknown_machine", "no machine with this id"};
    if (component.empty())
        return base::Error{"statechart.bad_component", "component name must be non-empty"};
    for (const RtRegion& rt_region : instance->regions) {
        if (rt_region.name != region)
            continue;
        for (std::uint32_t s = rt_region.first_state;
             s < rt_region.first_state + rt_region.state_count;
             ++s) {
            if (instance->states[s].name != state)
                continue;
            for (const ComponentHookSeat& seat : instance->component_hooks)
                if (seat.state == s && seat.component == component)
                    return base::Error{"statechart.duplicate_component",
                                       "this component is already registered on the state"};
            // Registration order IS attach order (component_hooks.h).
            instance->component_hooks.push_back(ComponentHookSeat{s, component, &hooks});
            return std::nullopt;
        }
        return base::Error{"statechart.unknown_state", "no such state in the region"};
    }
    return base::Error{"statechart.unknown_region", "no such region in the machine"};
}

std::optional<base::Error>
Statechart::set_var(MachineId machine, std::string_view name, expr::Value value) {
    MachineInstance* instance = find_machine(machine);
    if (instance == nullptr)
        return base::Error{"statechart.unknown_machine", "no machine with this id"};
    const int slot = instance->env.find(name);
    if (slot < 0)
        return base::Error{"statechart.unknown_var", "variable not declared in the machine"};
    if (instance->env.vars()[static_cast<std::size_t>(slot)].type != value.type)
        return base::Error{"statechart.var_type", "value type differs from the declaration"};
    instance->slots[static_cast<std::size_t>(slot)] = value;
    return std::nullopt;
}

namespace {

const RtRegion* find_region(const MachineInstance& instance, base::Name region) {
    for (const RtRegion& rt_region : instance.regions)
        if (rt_region.name == region)
            return &rt_region;
    return nullptr;
}

const RtState*
find_state(const MachineInstance& instance, const RtRegion& region, base::Name state) {
    for (std::uint32_t s = region.first_state; s < region.first_state + region.state_count; ++s)
        if (instance.states[s].name == state)
            return &instance.states[s];
    return nullptr;
}

} // namespace

bool Statechart::in_state(MachineId machine, base::Name region, base::Name state) const {
    const MachineInstance* instance = find_machine(machine);
    if (instance == nullptr)
        return false;
    const RtRegion* rt_region = find_region(*instance, region);
    if (rt_region == nullptr)
        return false;
    const RtState* rt_state = find_state(*instance, *rt_region, state);
    return rt_state != nullptr && rt_state->active;
}

base::Name Statechart::active_state(MachineId machine, base::Name region) const {
    const MachineInstance* instance = find_machine(machine);
    if (instance == nullptr)
        return {};
    const RtRegion* rt_region = find_region(*instance, region);
    if (rt_region == nullptr || rt_region->active == kInvalidIndex)
        return {};
    return instance->states[rt_region->active].name;
}

ecs::EntityRef Statechart::machine_root(MachineId machine) const {
    const MachineInstance* instance = find_machine(machine);
    return instance != nullptr ? instance->root : ecs::EntityRef{};
}

ecs::EntityRef
Statechart::state_entity(MachineId machine, base::Name region, base::Name state) const {
    const MachineInstance* instance = find_machine(machine);
    if (instance == nullptr)
        return {};
    const RtRegion* rt_region = find_region(*instance, region);
    if (rt_region == nullptr)
        return {};
    const RtState* rt_state = find_state(*instance, *rt_region, state);
    return rt_state != nullptr ? rt_state->entity : ecs::EntityRef{};
}

std::optional<base::Error> Statechart::finish_state(MachineId machine,
                                                    base::Name region,
                                                    base::Name state,
                                                    std::uint64_t cause_id) {
    MachineInstance* instance = find_machine(machine);
    if (instance == nullptr)
        return base::Error{"statechart.unknown_machine", "no machine with this id"};
    const RtRegion* rt_region = find_region(*instance, region);
    if (rt_region == nullptr)
        return base::Error{"statechart.unknown_region", "no such region in the machine"};
    const RtState* rt_state = find_state(*instance, *rt_region, state);
    if (rt_state == nullptr)
        return base::Error{"statechart.unknown_state", "no such state in the region"};
    if (!rt_state->active)
        return base::Error{"statechart.state_not_active",
                           "state.finished emits only from an active state"};
    // The "<state>.finished" family payload (D-BUILD-056): entity as the
    // runtime entity_ref wire shape (to_bits, D-BUILD-046), names verbatim.
    base::Json payload = base::Json::object();
    payload.set("entity", static_cast<std::int64_t>(instance->host.to_bits()));
    payload.set("region", rt_region->name.view());
    payload.set("state", rt_state->name.view());
    bus::TriggerResult result = bus_->trigger(
        bus::EventKey::entity(instance->host), rt_state->finished_event, payload, cause_id);
    return result.error;
}

void Statechart::run_update(double dt, std::uint64_t cause_id) {
    run_update_hooks(false, dt, cause_id);
}

} // namespace midday::statechart
