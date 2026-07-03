// core/statechart/transitions.cpp — the A.2 transition algorithm, INLINE on
// events, and the A.2.1 enter/exit chains (recursive — statechart.h header
// comment spells out the observable order; hook_order_test.cpp pins it
// against the normative text).
//
// Re-entrancy: hooks run cascades through the bus (nested dispatch, shared
// depth cap). Scratch frames are indexed by the bus cascade depth — dispatch
// runs at depth >= 1 and every nesting level owns its frame, so the exact
// state of an outer evaluation survives any cascade. The own-region stamp is
// set BEFORE the exit chain runs: a cascade can never transition the region
// again this tick (A.2 rule 5, the cycle breaker).

#include "core/base/json.h"
#include "core/ecs/world.h"
#include "core/hierarchy/hierarchy.h"
#include "core/journal/writer.h"
#include "core/statechart/instance.h"
#include "core/statechart/statechart.h"

#include <algorithm>
#include <span>
#include <string_view>
#include <utility>

namespace midday::statechart {

namespace detail {
void fatal_if(const char* what, const std::optional<base::Error>& error); // statechart.cpp
} // namespace detail

namespace {

std::string_view source_form(base::Name source) {
    return source.empty() ? std::string_view("any-state") : source.view();
}

} // namespace

void Statechart::on_machine_event(MachineId machine, const bus::EventView& event) {
    MachineInstance* instance = find_machine(machine);
    if (instance == nullptr || !machine_live(*instance))
        return;
    const std::uint32_t depth = bus_->cascade_depth();
    ScratchFrame& frame = scratch_[depth < scratch_.size() ? depth : scratch_.size() - 1];
    // Regions evaluate in declaration order, each fully (collect, pick,
    // execute, void) before the next — a cascade from region N's hooks may
    // mark region N+1 before it evaluates (A.2 rule 5: other regions, if
    // unmarked).
    for (std::uint32_t r = 0; r < instance->regions.size(); ++r) {
        if (instance->regions[r].transition_stamp == bus_->tick())
            void_marked_region(*instance, r, event);
        else
            evaluate_region(*instance, r, event, frame);
    }
}

bool Statechart::filter_passes(MachineInstance& instance,
                               std::uint32_t transition_index,
                               const bus::EventView& event) {
    const RtTransition& transition = instance.transitions[transition_index];
    if (!transition.filter.has_value())
        return true;
    const expr::EvalResult result =
        transition.filter->eval(std::span<const expr::Value>(instance.slots));
    if (result.status != expr::EvalStatus::kOk) {
        // A faulting filter is a failed candidate, never UB: journaled with
        // the expr fault code, pair skipped (program.h FAILURE class).
        journal_fault(instance,
                      instance.regions[instance.states[transition.target].region].name,
                      transition.source,
                      transition.event,
                      expr::to_error(result.status).code,
                      event.record_id);
        return false;
    }
    return result.value.u.b;
}

void Statechart::evaluate_region(MachineInstance& instance,
                                 std::uint32_t region_index,
                                 const bus::EventView& event,
                                 ScratchFrame& frame) {
    RtRegion& region = instance.regions[region_index];
    frame.candidates.clear();
    // A.2 rule 1 candidates in declaration order: any-state rules first,
    // then the ACTIVE CHAIN's pairs outermost-first (document order — a
    // parent's pairs are declared before its substates').
    for (std::uint32_t t = region.first_any; t < region.first_any + region.any_count; ++t)
        if (instance.transitions[t].event == event.event && filter_passes(instance, t, event))
            frame.candidates.push_back(t);
    for (std::uint32_t s = region.active; s != kInvalidIndex; s = instance.states[s].active_child) {
        const RtState& state = instance.states[s];
        for (std::uint32_t t = state.first_transition;
             t < state.first_transition + state.transition_count;
             ++t)
            if (instance.transitions[t].event == event.event && filter_passes(instance, t, event))
                frame.candidates.push_back(t);
    }
    if (frame.candidates.empty())
        return;
    // A.2 rule 2: highest priority; tie -> earliest in declaration order
    // (strictly-greater replacement keeps the first of equals).
    std::size_t winner = 0;
    for (std::size_t i = 1; i < frame.candidates.size(); ++i)
        if (instance.transitions[frame.candidates[i]].priority >
            instance.transitions[frame.candidates[winner]].priority)
            winner = i;
    const std::uint32_t winning_transition = frame.candidates[winner];
    // A.2 rule 3 then rule 4: execute inline, then journal the losers.
    // Cascades inside the execution use DEEPER scratch frames, so the
    // candidate list is intact when the losers journal.
    execute_transition(instance, region_index, winning_transition, event, frame);
    for (std::size_t i = 0; i < frame.candidates.size(); ++i)
        if (i != winner)
            journal_voided(instance, region_index, frame.candidates[i], "lost", event.record_id);
}

void Statechart::void_marked_region(MachineInstance& instance,
                                    std::uint32_t region_index,
                                    const bus::EventView& event) {
    // A.2 rule 4: on later events this tick, ALL name-matching pairs in the
    // region journal as voided — active or not (the A.3 trace voids the
    // exited SlashAttack's stagger.hit pair). Filters are not consulted:
    // the record explains why a matching RULE did not run.
    const RtRegion& region = instance.regions[region_index];
    for (std::uint32_t t = region.first_any; t < region.transitions_end; ++t)
        if (instance.transitions[t].event == event.event)
            journal_voided(
                instance, region_index, t, "region_already_transitioned", event.record_id);
}

void Statechart::execute_transition(MachineInstance& instance,
                                    std::uint32_t region_index,
                                    std::uint32_t transition_index,
                                    const bus::EventView& event,
                                    ScratchFrame& frame) {
    RtRegion& region = instance.regions[region_index];
    const RtTransition& transition = instance.transitions[transition_index];
    const std::uint32_t target = transition.target;

    // Target ancestor path, region-root side first.
    frame.enter_path.clear();
    for (std::uint32_t a = target; a != kInvalidIndex; a = instance.states[a].parent)
        frame.enter_path.push_back(a);
    std::ranges::reverse(frame.enter_path);

    // The transition domain: exit the active-chain child of the deepest
    // common ancestor of (active leaf, target); enter from its target-side
    // sibling down. A target inside the active chain exits and re-enters
    // itself (external self-transition — spec 4.1 re-entry semantics).
    std::uint32_t exit_root = kInvalidIndex;
    std::uint32_t enter_from = 0;
    if (region.active != kInvalidIndex) {
        if (frame.enter_path[0] != region.active) {
            exit_root = region.active;
        } else {
            std::uint32_t current = region.active;
            std::size_t step = 0;
            while (true) {
                if (current == target) { // ancestor-or-self of the leaf: re-entry
                    exit_root = target;
                    enter_from = static_cast<std::uint32_t>(frame.enter_path.size() - 1);
                    break;
                }
                const std::uint32_t next = instance.states[current].active_child;
                ++step;
                if (next == kInvalidIndex) { // pure descend below the active leaf
                    enter_from = static_cast<std::uint32_t>(step);
                    break;
                }
                if (frame.enter_path[step] != next) { // divergence: exit the chain side
                    exit_root = next;
                    enter_from = static_cast<std::uint32_t>(step);
                    break;
                }
                current = next;
            }
        }
    }

    const base::Name from =
        exit_root != kInvalidIndex ? instance.states[exit_root].name : base::Name();
    const base::Name to = instance.states[target].name;

    // Record before effect; every hook record chains from this id.
    base::Json payload = base::Json::object();
    payload.set("machine", instance.name.view());
    payload.set("entity", entity_form(instance.host));
    payload.set("region", region.name.view());
    payload.set("from", from.view());
    payload.set("to", to.view());
    payload.set("via", source_form(transition.source));
    const std::uint64_t record_id = journal_->record(bus_->tick(),
                                                     journal::Tier::Flight,
                                                     "statechart.transition",
                                                     event.record_id,
                                                     std::move(payload));
    // Mark BEFORE any hook runs: cascades can never re-transition this
    // region this tick (A.2 rules 1 and 5).
    region.transition_stamp = bus_->tick();
    stats_.transitions += 1;

    if (exit_root != kInvalidIndex) {
        const std::uint32_t parent = instance.states[exit_root].parent;
        exit_state(instance, exit_root, to, record_id);
        if (parent != kInvalidIndex) {
            instance.states[parent].active_child = kInvalidIndex;
        } else {
            region.active = kInvalidIndex;
            if (region.history)
                region.history_state = exit_root; // consumed by machine-as-substate (M1)
        }
    }
    enter_state(instance,
                frame.enter_path[enter_from],
                frame.enter_path,
                enter_from,
                from,
                record_id,
                /*initial_entry=*/false);
}

void Statechart::exit_state(MachineInstance& instance,
                            std::uint32_t state_index,
                            base::Name to,
                            std::uint64_t cause_id) {
    RtState& state = instance.states[state_index];
    // A.2.1 exit 1 — the state script, while its parts are still live.
    if (state.hooks != nullptr) {
        const std::uint64_t record_id =
            journal_hook(instance, state_index, "exit", to, cause_id, journal::Tier::Flight);
        stats_.hook_calls += 1;
        state.hooks->on_exit(*this, hook_context(instance, state_index, to, 0.0, record_id));
    }
    // A.2.1 exit 2 — the active substate exits (recursively, deepest
    // completing first). Open sequence spans close here at m0-sequences.
    if (state.active_child != kInvalidIndex) {
        if (state.history)
            state.history_child = state.active_child; // resume point (spec 4.1)
        const std::uint32_t child = state.active_child;
        exit_state(instance, child, to, cause_id);
        state.active_child = kInvalidIndex;
    }
    // A.2.1 exit 3 — components onExit: script components attach with the
    // bindings node; nothing to run C++-side yet.
    // Watcher re-arm rides the exit (edge semantics, statechart.h).
    for (std::uint32_t w = state.first_watcher; w < state.first_watcher + state.watcher_count; ++w)
        instance.watchers[w].armed_value = false;
    // A.2.1 exit 4 — the node subtree deactivates (exact per-pool pattern
    // capture, D-BUILD-030). An inactive state ALWAYS owns one scope.
    detail::fatal_if("deactivate exited state", hierarchy_->deactivate(state.entity));
    state.active = false;
    // A.2.1 exit 5 — sequence playhead reset/save: m0-sequences attaches.
}

void Statechart::enter_state(MachineInstance& instance,
                             std::uint32_t state_index,
                             const std::vector<std::uint32_t>& path,
                             std::uint32_t path_pos,
                             base::Name from,
                             std::uint64_t cause_id,
                             bool initial_entry) {
    RtState& state = instance.states[state_index];
    // A.2.1 enter 1 — the subtree activates (pattern restore). At
    // instantiate the initial chain was built active and owns no scope.
    if (!initial_entry)
        detail::fatal_if("activate entered state", hierarchy_->activate(state.entity));
    state.active = true;
    if (state.parent != kInvalidIndex)
        instance.states[state.parent].active_child = state_index;
    else
        instance.regions[state.region].active = state_index;
    // A.2.1 enter 2 — components onEnter: attaches with the bindings node.
    // A.2.1 enter 3 — the substate enters: the forced path toward a deep
    // target wins over history over initial.
    std::uint32_t next = kInvalidIndex;
    if (static_cast<std::size_t>(path_pos) + 1 < path.size())
        next = path[path_pos + 1];
    else if (state.history && state.history_child != kInvalidIndex)
        next = state.history_child;
    else
        next = state.initial_child;
    if (next != kInvalidIndex)
        enter_state(instance, next, path, path_pos + 1, from, cause_id, initial_entry);
    // A.2.1 enter 4 — the state script, LAST, when its parts are live.
    if (state.hooks != nullptr) {
        const std::uint64_t record_id =
            journal_hook(instance, state_index, "enter", from, cause_id, journal::Tier::Flight);
        stats_.hook_calls += 1;
        state.hooks->on_enter(*this, hook_context(instance, state_index, from, 0.0, record_id));
    }
}

std::uint64_t Statechart::journal_hook(MachineInstance& instance,
                                       std::uint32_t state_index,
                                       std::string_view hook,
                                       base::Name peer,
                                       std::uint64_t cause_id,
                                       journal::Tier tier) {
    const RtState& state = instance.states[state_index];
    base::Json payload = base::Json::object();
    payload.set("machine", instance.name.view());
    payload.set("entity", entity_form(instance.host));
    payload.set("region", instance.regions[state.region].name.view());
    payload.set("state", state.name.view());
    payload.set("hook", hook);
    if (!peer.empty())
        payload.set("peer", peer.view());
    return journal_->record(bus_->tick(), tier, "statechart.hook", cause_id, std::move(payload));
}

void Statechart::journal_voided(MachineInstance& instance,
                                std::uint32_t region_index,
                                std::uint32_t transition_index,
                                std::string_view reason,
                                std::uint64_t cause_id) {
    const RtTransition& transition = instance.transitions[transition_index];
    base::Json payload = base::Json::object();
    payload.set("machine", instance.name.view());
    payload.set("entity", entity_form(instance.host));
    payload.set("region", instance.regions[region_index].name.view());
    payload.set("event", transition.event.view());
    payload.set("source", source_form(transition.source));
    payload.set("target", instance.states[transition.target].name.view());
    payload.set("priority", static_cast<std::int64_t>(transition.priority));
    payload.set("reason", reason);
    journal_->record(
        bus_->tick(), journal::Tier::Flight, "statechart.voided", cause_id, std::move(payload));
    stats_.voided += 1;
}

void Statechart::journal_fault(MachineInstance& instance,
                               base::Name region,
                               base::Name source,
                               base::Name event,
                               std::string_view code,
                               std::uint64_t cause_id) {
    base::Json payload = base::Json::object();
    payload.set("machine", instance.name.view());
    payload.set("entity", entity_form(instance.host));
    payload.set("region", region.view());
    payload.set("source", source_form(source));
    payload.set("event", event.view());
    payload.set("error", code);
    journal_->record(bus_->tick(),
                     journal::Tier::Flight,
                     "statechart.filter_fault",
                     cause_id,
                     std::move(payload));
    stats_.filter_faults += 1;
}

} // namespace midday::statechart
