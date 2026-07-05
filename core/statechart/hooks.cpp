// core/statechart/hooks.cpp — update-flavor hook driving (A.1 phase 5 for
// onFixedUpdate; run_update() for the frame-side onUpdate) and the shared
// hook context builder.
//
// Order (A.1 phase 5, normative): entities in scene-tree depth-first order
// (machines by their root's tree order), regions in declaration order, the
// active chain outermost-first — state script first; script COMPONENTS join
// the same slots at the bindings node. Update-flavor hook records are TRACE
// tier: per-tick volume does not belong in the always-on causality skeleton,
// and the consumed ids keep FLIGHT bytes invariant (D-BUILD-032).

#include "core/ecs/world.h"
#include "core/hierarchy/hierarchy.h"
#include "core/statechart/instance.h"
#include "core/statechart/statechart.h"

#include <algorithm>

namespace midday::statechart {

StateHookContext Statechart::hook_context(MachineInstance& instance,
                                          std::uint32_t state_index,
                                          base::Name peer,
                                          double dt,
                                          std::uint64_t record_id) const {
    const RtState& state = instance.states[state_index];
    StateHookContext context;
    context.machine = instance.id;
    context.host = instance.host;
    context.state_entity = state.entity;
    context.region = instance.regions[state.region].name;
    context.state = state.name;
    context.peer = peer;
    context.dt = dt;
    context.record_id = record_id;
    context.tick = bus_->tick();
    return context;
}

void Statechart::run_update_hooks(bool fixed, double dt, std::uint64_t cause_id) {
    machine_order_.clear();
    for (MachineId m = 0; m < machines_.size(); ++m) {
        MachineInstance& instance = *machines_[m];
        if (!machine_live(instance))
            continue;
        if (hierarchy_->is_dormant(instance.root))
            continue; // dormant machines do not tick (spec 4.1)
        machine_order_.emplace_back(hierarchy_->order_index(instance.root).value_or(0xFFFFFFFFU),
                                    m);
    }
    std::ranges::sort(machine_order_);

    for (const auto& [order, m] : machine_order_) {
        MachineInstance& instance = *machines_[m];
        if (!machine_live(instance) || hierarchy_->is_dormant(instance.root))
            continue;
        for (RtRegion& region : instance.regions) {
            // Snapshot the active chain: a hook's cascade may transition
            // this very region mid-walk (first transition of the tick is
            // legal from an update hook — A.2 runs inline anywhere).
            std::vector<std::uint32_t>& chain = scratch_[bus_->cascade_depth()].enter_path;
            chain.clear();
            for (std::uint32_t s = region.active; s != kInvalidIndex;
                 s = instance.states[s].active_child)
                chain.push_back(s);
            for (const std::uint32_t s : chain) {
                RtState& state = instance.states[s];
                if (!state.active || state.hooks == nullptr)
                    continue; // exited by an earlier hook's cascade
                const std::uint64_t record_id = journal_hook(instance,
                                                             s,
                                                             fixed ? "fixed-update" : "update",
                                                             base::Name(),
                                                             cause_id,
                                                             journal::Tier::Trace);
                const StateHookContext context =
                    hook_context(instance, s, base::Name(), dt, record_id);
                if (fixed)
                    state.hooks->on_fixed_update(*this, context);
                else
                    state.hooks->on_update(*this, context);
            }
        }
    }
}

} // namespace midday::statechart
