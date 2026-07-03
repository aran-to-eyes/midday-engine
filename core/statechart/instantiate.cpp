// core/statechart/instantiate.cpp — MachineDesc -> live machine subtree.
//
// Two strict phases (atomic instantiate — a refused description mutates
// NOTHING):
//   1. validate_and_compile: every reference resolved, every filter/watcher
//      compiled and bool-typed, the whole runtime table staged off-world.
//   2. build + enter: spawn the subtree (root under host, regions under
//      root, states under regions/parent states — document order IS attach
//      order, so tree order is deterministic), flush the structural queue
//      (instantiate is a BOOT/STRUCTURAL-PHASE operation, refused
//      mid-iteration by the ECS lock), deactivate every non-initial state
//      (each inactive state owns exactly one activation scope — the
//      invariant transitions maintain, D-BUILD-030 nesting), journal,
//      subscribe, and run the initial enter chains (A.1 phase 8: "spawned
//      entities go live: initial states enter").

#include "core/base/error.h"
#include "core/base/json.h"
#include "core/bus/entity_listener.h"
#include "core/ecs/world.h"
#include "core/expr/expr.h"
#include "core/hierarchy/hierarchy.h"
#include "core/journal/writer.h"
#include "core/statechart/instance.h"
#include "core/statechart/statechart.h"

#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace midday::statechart {

namespace detail {
void fatal_if(const char* what, const std::optional<base::Error>& error); // statechart.cpp
} // namespace detail

namespace {

base::Error make_error(std::string_view code, std::string_view message, std::string_view where) {
    base::Error error;
    error.code = std::string(code);
    error.message = std::string(message);
    if (!where.empty())
        error.details.set("where", where);
    return error;
}

// Compile one boolean expression (filter or watcher condition) against the
// machine environment; diagnostics carry the authoring origin.
struct CompiledBool {
    std::optional<expr::Program> program;
    std::optional<base::Error> error;
};

CompiledBool compile_bool(const std::string& source,
                          const expr::EnvSpec& env,
                          const std::string& origin,
                          std::string_view bad_code,
                          std::string_view not_bool_code) {
    CompiledBool out;
    expr::CompileResult compiled = expr::compile(source, env, origin);
    if (compiled.diag.has_value()) {
        const base::Error error = compiled.diag->to_error();
        base::Error wrapped;
        wrapped.code = std::string(bad_code);
        wrapped.message = error.message;
        wrapped.details = error.details;
        wrapped.details.set("expr_code", error.code);
        out.error = std::move(wrapped);
        return out;
    }
    if (!compiled.program.has_value() ||
        compiled.program->result_type() != expr::ValueType::kBool) {
        out.error = make_error(not_bool_code, "expression must evaluate to bool", origin);
        return out;
    }
    out.program = std::move(compiled.program);
    return out;
}

} // namespace

std::optional<base::Error> Statechart::validate_and_compile(const MachineDesc& desc,
                                                            MachineInstance& staged) const {
    if (desc.name.empty())
        return make_error("statechart.bad_machine", "machine name must be non-empty", {});
    staged.name = desc.name;
    if (desc.regions.empty())
        return make_error(
            "statechart.empty_machine", "a machine needs at least one region", desc.name.view());

    // Environment: slot = declaration index (machine_desc.h contract).
    for (const VarDesc& var : desc.vars) {
        if (var.name.empty())
            return make_error(
                "statechart.bad_var", "variable name must be non-empty", desc.name.view());
        if (staged.env.find(var.name) >= 0)
            return make_error("statechart.duplicate_var", "variable declared twice", var.name);
        staged.env.declare(var.name, var.type);
        staged.slots.push_back(zero_value(var.type));
    }

    for (std::size_t i = 0; i < desc.channels.size(); ++i) {
        if (desc.channels[i].empty())
            return make_error(
                "statechart.bad_channel", "channel name must be non-empty", desc.name.view());
        for (std::size_t j = 0; j < i; ++j)
            if (desc.channels[j] == desc.channels[i])
                return make_error(
                    "statechart.bad_channel", "channel declared twice", desc.channels[i].view());
    }

    // Pass 1 — flatten states region by region, document order (parents
    // before substates: the attach order AND the void-scan order).
    std::vector<const StateDesc*> sources; // parallel to staged.states
    std::optional<base::Error> failure;
    for (const RegionDesc& region_desc : desc.regions) {
        if (region_desc.name.empty())
            return make_error(
                "statechart.bad_region", "region name must be non-empty", desc.name.view());
        for (const RtRegion& prior : staged.regions)
            if (prior.name == region_desc.name)
                return make_error("statechart.duplicate_region",
                                  "region declared twice",
                                  region_desc.name.view());
        if (region_desc.states.empty())
            return make_error("statechart.empty_region",
                              "a region needs at least one state",
                              region_desc.name.view());

        RtRegion region;
        region.name = region_desc.name;
        region.history = region_desc.history;
        region.first_state = static_cast<std::uint32_t>(staged.states.size());
        const auto region_index = static_cast<std::uint32_t>(staged.regions.size());

        auto flatten =
            [&](auto&& self, const StateDesc& state_desc, std::uint32_t parent) -> std::uint32_t {
            if (failure.has_value())
                return kInvalidIndex;
            if (state_desc.name.empty()) {
                failure = make_error("statechart.bad_state",
                                     "state name must be non-empty",
                                     region_desc.name.view());
                return kInvalidIndex;
            }
            for (std::size_t s = region.first_state; s < staged.states.size(); ++s) {
                if (staged.states[s].name == state_desc.name) {
                    failure = make_error("statechart.duplicate_state",
                                         "state name declared twice in the region",
                                         state_desc.name.view());
                    return kInvalidIndex;
                }
            }
            const auto index = static_cast<std::uint32_t>(staged.states.size());
            RtState state;
            state.name = state_desc.name;
            state.finished_event = base::Name(std::string(state_desc.name.view()) + ".finished");
            state.region = region_index;
            state.parent = parent;
            state.history = state_desc.history;
            staged.states.push_back(state); // trivially copyable
            sources.push_back(&state_desc);
            if (!state_desc.substates.empty() && state_desc.initial.empty()) {
                failure = make_error("statechart.bad_initial",
                                     "a state with substates must declare initial",
                                     state_desc.name.view());
                return kInvalidIndex;
            }
            for (const StateDesc& substate : state_desc.substates) {
                const std::uint32_t child = self(self, substate, index);
                if (failure.has_value())
                    return kInvalidIndex;
                if (substate.name == state_desc.initial)
                    staged.states[index].initial_child = child;
            }
            if (!state_desc.substates.empty() &&
                staged.states[index].initial_child == kInvalidIndex) {
                failure = make_error("statechart.bad_initial",
                                     "initial must name a direct substate",
                                     state_desc.name.view());
                return kInvalidIndex;
            }
            return index;
        };

        for (const StateDesc& state_desc : region_desc.states) {
            const std::uint32_t top = flatten(flatten, state_desc, kInvalidIndex);
            if (failure.has_value())
                return failure;
            if (state_desc.name == region_desc.initial)
                region.initial = top;
        }
        region.state_count = static_cast<std::uint32_t>(staged.states.size()) - region.first_state;
        if (region.initial == kInvalidIndex)
            return make_error("statechart.bad_initial",
                              "region initial must name a top-level state",
                              region_desc.name.view());
        staged.regions.push_back(region);
    }

    // Pass 2 — transitions and watchers, now that every target resolves.
    // Layout: per region, any-state rules FIRST, then each state's pairs in
    // document order — [first_any, transitions_end) IS the region's A.2
    // declaration order (instance.h).
    for (std::size_t r = 0; r < desc.regions.size(); ++r) {
        const RegionDesc& region_desc = desc.regions[r];
        RtRegion& region = staged.regions[r];

        auto resolve_target = [&](base::Name target) -> std::uint32_t {
            for (std::uint32_t s = region.first_state; s < region.first_state + region.state_count;
                 ++s)
                if (staged.states[s].name == target)
                    return s;
            return kInvalidIndex;
        };
        auto append_transition = [&](const TransitionDesc& pair,
                                     base::Name source,
                                     const std::string& origin) -> std::optional<base::Error> {
            if (pair.event.empty() || pair.target.empty())
                return make_error("statechart.bad_transition",
                                  "transition needs both event and goto target",
                                  origin);
            const std::uint32_t target = resolve_target(pair.target);
            if (target == kInvalidIndex)
                return make_error("statechart.bad_target",
                                  "goto target is not a state of this region",
                                  pair.target.view());
            RtTransition transition;
            transition.event = pair.event;
            transition.source = source;
            transition.target = target;
            transition.priority = pair.priority;
            if (!pair.condition.empty()) {
                CompiledBool compiled = compile_bool(pair.condition,
                                                     staged.env,
                                                     origin,
                                                     "statechart.bad_filter",
                                                     "statechart.filter_not_bool");
                if (compiled.error.has_value())
                    return compiled.error;
                transition.filter = std::move(compiled.program);
            }
            staged.transitions.push_back(std::move(transition));
            return std::nullopt;
        };

        const std::string region_origin =
            std::string(desc.name.view()) + "/" + std::string(region_desc.name.view());
        region.first_any = static_cast<std::uint32_t>(staged.transitions.size());
        for (const TransitionDesc& pair : region_desc.any_state)
            if (auto error = append_transition(pair, base::Name(), region_origin + "/any-state"))
                return error;
        region.any_count = static_cast<std::uint32_t>(staged.transitions.size()) - region.first_any;

        for (std::uint32_t s = region.first_state; s < region.first_state + region.state_count;
             ++s) {
            RtState& state = staged.states[s];
            const StateDesc& source = *sources[s];
            const std::string origin = region_origin + "/" + std::string(state.name.view());
            state.first_transition = static_cast<std::uint32_t>(staged.transitions.size());
            for (const TransitionDesc& pair : source.transitions)
                if (auto error = append_transition(pair, state.name, origin))
                    return error;
            state.transition_count =
                static_cast<std::uint32_t>(staged.transitions.size()) - state.first_transition;

            state.first_watcher = static_cast<std::uint32_t>(staged.watchers.size());
            for (const WatcherDesc& watcher_desc : source.watchers) {
                if (watcher_desc.condition.empty() || watcher_desc.event.empty())
                    return make_error(
                        "statechart.bad_watcher", "watcher needs both condition and event", origin);
                CompiledBool compiled = compile_bool(watcher_desc.condition,
                                                     staged.env,
                                                     origin + "/when",
                                                     "statechart.bad_watcher",
                                                     "statechart.watcher_not_bool");
                if (compiled.error.has_value() || !compiled.program.has_value())
                    return compiled.error;
                RtWatcher watcher{std::move(*compiled.program), watcher_desc.event, s, false};
                staged.watchers.push_back(std::move(watcher));
            }
            state.watcher_count =
                static_cast<std::uint32_t>(staged.watchers.size()) - state.first_watcher;
        }
        region.transitions_end = static_cast<std::uint32_t>(staged.transitions.size());
    }
    return std::nullopt;
}

std::optional<base::Error>
Statechart::build_subtree(MachineInstance& staged, ecs::EntityRef host, MachineId id) {
    std::vector<ecs::EntityRef> spawned;
    auto fail = [&](base::Error error) -> std::optional<base::Error> {
        // Best-effort rollback: despawn in reverse spawn order (an adopted
        // root cascades its subtree; the rest are individual no-ops then).
        for (const ecs::EntityRef entity : std::ranges::reverse_view(spawned))
            if (world_->alive(entity))
                (void)world_->despawn(entity);
        return error;
    };
    auto spawn_one = [&](ecs::EntityRef& out) -> std::optional<base::Error> {
        base::Error error;
        out = world_->spawn(&error);
        if (out.is_null())
            return error;
        spawned.push_back(out);
        return std::nullopt;
    };

    if (auto error = spawn_one(staged.root))
        return fail(*error);
    if (auto error = world_->emplace<MachineRoot>(staged.root, MachineRoot{this, id}))
        return fail(*error);
    if (auto error = hierarchy_->queue_attach(staged.root, host))
        return fail(*error);

    for (std::uint32_t r = 0; r < staged.regions.size(); ++r) {
        RtRegion& region = staged.regions[r];
        if (auto error = spawn_one(region.entity))
            return fail(*error);
        if (auto error = hierarchy_->queue_attach(region.entity, staged.root))
            return fail(*error);
        for (std::uint32_t s = region.first_state; s < region.first_state + region.state_count;
             ++s) {
            RtState& state = staged.states[s];
            if (auto error = spawn_one(state.entity))
                return fail(*error);
            StateNode node;
            node.machine = staged.name;
            node.region = region.name;
            node.state = state.name;
            node.machine_id = id;
            node.state_index = s;
            if (auto error = world_->emplace<StateNode>(state.entity, node))
                return fail(*error);
            const ecs::EntityRef parent_entity =
                state.parent != kInvalidIndex ? staged.states[state.parent].entity : region.entity;
            if (auto error = hierarchy_->queue_attach(state.entity, parent_entity))
                return fail(*error);
        }
    }

    if (auto error = world_->flush_structural())
        return fail(*error);
    // The machine subtree is an ownership boundary (spec 4.1: machines are
    // prefab subtrees; hierarchy owner markers).
    if (auto error = hierarchy_->set_owner(staged.root, true))
        return fail(*error);
    return std::nullopt;
}

InstantiateResult
Statechart::instantiate(const MachineDesc& desc, ecs::EntityRef host, std::uint64_t cause_id) {
    InstantiateResult result;
    if (auto error = world_->check_alive(host)) {
        result.error = error;
        return result;
    }
    if (!hierarchy_->contains(host)) {
        result.error =
            make_error("statechart.host_unadopted", "the host entity must be in the hierarchy", {});
        return result;
    }

    auto staged = std::make_unique<MachineInstance>();
    staged->host = host;
    if (auto error = validate_and_compile(desc, *staged)) {
        result.error = error;
        return result;
    }
    const auto id = static_cast<MachineId>(machines_.size());
    staged->id = id;
    if (auto error = build_subtree(*staged, host, id)) {
        result.error = error;
        return result;
    }

    // Initial configuration: each region's initial chain is active; EVERY
    // other state owns one deactivation scope (the transition invariant).
    for (RtRegion& region : staged->regions) {
        std::uint32_t current = region.initial;
        region.active = current;
        staged->states[current].active = true;
        while (staged->states[current].initial_child != kInvalidIndex) {
            const std::uint32_t child = staged->states[current].initial_child;
            staged->states[current].active_child = child;
            staged->states[child].active = true;
            current = child;
        }
    }
    for (const RtState& state : staged->states)
        if (!state.active)
            detail::fatal_if("deactivate initial-dormant state",
                             hierarchy_->deactivate(state.entity));

    // Journal BEFORE any effect (the bus discipline): unjournaled machines
    // do not exist.
    base::Json payload = base::Json::object();
    payload.set("machine", staged->name.view());
    payload.set("entity", entity_form(host));
    payload.set("root", entity_form(staged->root));
    const std::uint64_t record_id = journal_->record(bus_->tick(),
                                                     journal::Tier::Flight,
                                                     "statechart.instantiate",
                                                     cause_id,
                                                     std::move(payload));
    if (record_id == 0) {
        base::Error error = make_error(
            "statechart.journal_refused", "the journal refused the instantiate record", {});
        const std::optional<base::Error>& status = journal_->status();
        if (status.has_value())
            error.details.set("journal", status->to_json());
        (void)world_->despawn(staged->root); // cascades the whole subtree
        result.error = error;
        return result;
    }

    // Subscribe the root's MachineRoot row: host private channel + declared
    // named channels (D-BUILD-046 key-only channels; the tables filter).
    staged->keys.push_back(bus::EventKey::entity(host));
    for (const base::Name& channel : desc.channels)
        staged->keys.push_back(bus::EventKey::named(channel));
    for (const bus::EventKey& key : staged->keys)
        detail::fatal_if("subscribe machine root",
                         bus::subscribe_component<MachineRoot>(*bus_, key, staged->root));

    machines_.push_back(std::move(staged));
    stats_.machines += 1;

    // Initial enter chains, region declaration order (A.1 phase 8). Hooks
    // registered later see nothing retroactively; a cascade from an enter
    // hook may legally transition a LATER region first — its initial entry
    // is then skipped (the region already moved on).
    MachineInstance& instance = *machines_[id];
    const std::vector<std::uint32_t> no_path;
    for (RtRegion& region : instance.regions) {
        if (region.transition_stamp != kNeverTicked)
            continue;
        enter_state(instance,
                    region.initial,
                    no_path,
                    0,
                    base::Name(),
                    record_id,
                    /*initial_entry=*/true);
    }
    result.machine = id;
    return result;
}

} // namespace midday::statechart
