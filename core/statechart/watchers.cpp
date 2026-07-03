// core/statechart/watchers.cpp — A.1 phase 3: `when:` condition watchers.
//
// Order (normative): hierarchy tree order of the OWNING STATE's entity, then
// declaration order — collected fresh each phase into a reused buffer (the
// only steady-state cost is the sort; topology may change between ticks).
// Edge semantics (statechart.h): armed false at state entry, fires once on
// observing true, silent while true, re-arms on observing false or on state
// exit. The armed value updates BEFORE the trigger so an exit re-arm caused
// by the fired event's own cascade is never clobbered.

#include "core/base/json.h"
#include "core/ecs/world.h"
#include "core/hierarchy/hierarchy.h"
#include "core/statechart/instance.h"
#include "core/statechart/statechart.h"

#include <algorithm>
#include <span>

namespace midday::statechart {

void Statechart::run_watchers(const tick::PhaseContext& context) {
    watcher_order_.clear();
    for (MachineId m = 0; m < machines_.size(); ++m) {
        MachineInstance& instance = *machines_[m];
        if (instance.watchers.empty() || !machine_live(instance))
            continue;
        if (hierarchy_->is_dormant(instance.root))
            continue; // dormant parts see nothing (spec 4.1)
        for (std::uint32_t w = 0; w < instance.watchers.size(); ++w) {
            const RtWatcher& watcher = instance.watchers[w];
            if (!instance.states[watcher.state].active)
                continue;
            const std::uint64_t order =
                hierarchy_->order_index(instance.states[watcher.state].entity)
                    .value_or(0xFFFFFFFFU);
            watcher_order_.emplace_back((order << 32U) | w, m);
        }
    }
    std::ranges::sort(watcher_order_);

    for (const auto& [key, m] : watcher_order_) {
        const auto w = static_cast<std::uint32_t>(key & 0xFFFFFFFFU);
        MachineInstance& instance = *machines_[m];
        RtWatcher& watcher = instance.watchers[w];
        RtState& state = instance.states[watcher.state];
        // Re-check: an earlier watcher's event may have transitioned this
        // state away or retired the machine mid-phase.
        if (!machine_live(instance) || !state.active || hierarchy_->is_dormant(instance.root))
            continue;
        const expr::EvalResult result =
            watcher.program.eval(std::span<const expr::Value>(instance.slots));
        if (result.status != expr::EvalStatus::kOk) {
            // Faulting evaluation: journaled, skipped, arm state untouched.
            journal_fault(instance,
                          instance.regions[state.region].name,
                          state.name,
                          watcher.event,
                          expr::to_error(result.status).code,
                          context.phase_record_id);
            continue;
        }
        const bool value = result.value.u.b;
        const bool fire = value && !watcher.armed_value;
        watcher.armed_value = value;
        if (fire) {
            stats_.watcher_fires += 1;
            // Cause = THE phase marker (engine-initiated effect, D-BUILD-050).
            // A refused trigger already journaled its refusal at the bus.
            (void)bus_->trigger(bus::EventKey::entity(instance.host),
                                watcher.event,
                                base::Json::object(),
                                context.phase_record_id);
        }
    }
}

} // namespace midday::statechart
