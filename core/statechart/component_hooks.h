// core/statechart/component_hooks.h — the state-component hook seam (M2 0B,
// #12b): the A.2.1 exit-3 / enter-2 slots ("S's components onExit — reverse
// attach order" / "S's components onEnter — attach order") get their real
// invocation target. A state's component set (StateDesc::components, spec
// 4.1 "states owning component sets") is materialized by a HOST outside
// core/ (ts/runtime's ComponentInstanceHost for TS components); the chart
// only ever stores this narrow pure-virtual interface — zero core -> ts
// dependency, the exact StateHooks precedent one slot over.
//
// Registration (Statechart::add_component_hooks, statechart.h): one
// registration per (machine, region, state, component). REGISTRATION ORDER
// IS ATTACH ORDER — enter-2 runs the state's registrations first-to-last,
// exit-3 runs them last-to-first. Duplicate (state, component) registration
// refuses ("statechart.duplicate_component"); hooks registered after a
// state entered see nothing retroactively (the set_state_hooks contract).
//
// Journal: every invocation writes a FLIGHT "statechart.hook" record BEFORE
// the hook runs — hook: "component_enter" | "component_exit" plus the
// component name — citing the owning transition/instantiate record, exactly
// like the state-script hook records around it. The record id rides the
// context as THE cause id for the hook's effects.
//
// Lifetime: the chart stores the pointer and never owns it — hooks must
// have stable addresses and outlive the machine (or the Statechart), the
// StateHooks contract verbatim.

#pragma once

#include "core/base/name.h"
#include "core/ecs/entity.h"

#include <cstdint>

namespace midday::statechart {

class Statechart;

// The machine-instance id vocabulary (HOME here; statechart.h re-exposes it
// by including this header — one definition, no ODR hazard).
using MachineId = std::uint32_t;
inline constexpr MachineId kInvalidMachine = 0xFFFFFFFFU;

// What every component hook invocation receives (the StateHookContext shape
// plus the component's own name).
struct ComponentHookContext {
    MachineId machine = kInvalidMachine;
    ecs::EntityRef host;         // the entity owning the machine
    ecs::EntityRef state_entity; // the owning state's hierarchy node
    base::Name region;
    base::Name state;
    base::Name component; // the component's type name (attach identity)
    // Transition peer: the `to` state for on_exit, the `from` state for
    // on_enter (empty at initial entry).
    base::Name peer;
    std::uint64_t record_id = 0; // journal id of THIS hook's record — THE
                                 // cause id for effects (bus triggers)
    std::uint64_t tick = 0;
};

// The per-component C++ hook interface. Both hooks are pure: a materializer
// that registers a component seat always owns both lifecycle edges (a
// component with neither hook simply never registers here — attach and
// event dispatch live on the host side).
class ComponentHooks {
public:
    ComponentHooks() = default;
    ComponentHooks(const ComponentHooks&) = default;
    ComponentHooks& operator=(const ComponentHooks&) = default;
    ComponentHooks(ComponentHooks&&) = default;
    ComponentHooks& operator=(ComponentHooks&&) = default;

    virtual void on_enter(Statechart& chart, const ComponentHookContext& context) = 0;
    virtual void on_exit(Statechart& chart, const ComponentHookContext& context) = 0;

protected:
    ~ComponentHooks() = default; // never deleted through this interface
};

} // namespace midday::statechart
