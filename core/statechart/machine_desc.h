// core/statechart/machine_desc.h — the data-driven machine description
// (m0-statechart-core): plain aggregates with Name-based references, exactly
// the shape a YAML machine file parses into (m0-yaml-loader-run builds these
// from authored text; C++ fixtures build them directly — same path either way,
// spec section 4.1 "machines are prefab subtrees" arrives at m1-prefab-spawn).
//
// Reference rules (validated at Statechart::instantiate, never trusted):
//   * State names are unique within their REGION (all nesting levels — `goto`
//     targets resolve region-wide by name, spec 4.2 override-path grammar
//     addresses states by name, never index).
//   * `initial` names a direct substate (of the region / of the parent state)
//     and is required wherever substates exist.
//   * TransitionDesc::target may name ANY state of the same region, at any
//     depth: entering a nested target enters its ancestor chain first, then
//     descends per initial/history below the target (A.2.1 enter order).
//   * `history: true` on a region/parent state resumes the LAST ACTIVE direct
//     substate on re-entry (deep history = flagging every level); default
//     re-entry starts at `initial` (spec 4.1 entry semantics).
//
// The expression environment (transition `if:` filters and `when:` watchers,
// D-BUILD-053): a machine's environment is EXACTLY its `vars` list — slot
// index = declaration index, one core/expr EnvSpec shared by every filter and
// watcher of the machine, compiled once at instantiate. Hosts bind values via
// Statechart::set_var (the loader later binds component fields the same way);
// unbound slots hold the type's zero value. Filters and watchers must
// typecheck to bool. String-typed vars must outlive every eval that sees them
// (core/expr/env.h contract).

#pragma once

#include "core/base/name.h"
#include "core/expr/value.h"

#include <cstdint>
#include <string>
#include <vector>

namespace midday::statechart {

// One declared expression-environment variable (slot = declaration index).
struct VarDesc {
    std::string name; // dotted spellings are first-class ("health.current")
    expr::ValueType type = expr::ValueType::kFloat;
};

// One `{event, goto, priority, if}` pair (spec 4.2). Declaration order inside
// the owning list is the A.2 tie-break order.
struct TransitionDesc {
    base::Name event;          // triggering event name (required)
    base::Name target;         // `goto:` state name in the same region (required)
    std::int32_t priority = 0; // higher wins; tie -> declaration order
    std::string condition;     // `if:` filter source; empty = unconditional
};

// One `when:` condition watcher (spec 4.2, A.1 phase 3): an edge-triggered
// boolean expression that fires `event` on the machine's host channel when it
// goes false -> true while the owning state is active.
struct WatcherDesc {
    std::string condition; // boolean expression over the machine environment
    base::Name event;      // the internal event fired on the rising edge
};

struct StateDesc {
    base::Name name;
    std::vector<TransitionDesc> transitions; // declaration order (A.2 rule 2)
    std::vector<WatcherDesc> watchers;
    std::vector<StateDesc> substates; // document order = sibling attach order
    base::Name initial;               // required iff substates is non-empty
    bool history = false;             // resume last active substate on re-entry
};

// A top-level parallel region: one active state at a time, independent
// transition marking (A.2 rule 1). Region-level `any_state` rules compete in
// the same priority space and tie-break as declared BEFORE every state pair.
struct RegionDesc {
    base::Name name;
    base::Name initial;
    bool history = false;
    std::vector<TransitionDesc> any_state;
    std::vector<StateDesc> states;
};

struct MachineDesc {
    base::Name name;
    std::vector<VarDesc> vars; // THE expression environment (header comment)
    // Named broadcast/group channels this machine also listens on; the host
    // entity's private channel is always subscribed (spec 4.2 key vocabulary —
    // symbolic self/root/global resolution is a loader concern).
    std::vector<base::Name> channels;
    std::vector<RegionDesc> regions; // declaration order = evaluation order
};

} // namespace midday::statechart
