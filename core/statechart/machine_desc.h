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

#include "core/base/json.h"
#include "core/base/name.h"
#include "core/expr/value.h"

#include <cstdint>
#include <optional>
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

// ---- sequences: dope-sheet states (spec 4.1, m0-sequences) ------------------
// A sequence is a STATE whose body is a timeline: trigger tracks fire bus
// events at a time, span tracks are named intervals that open/close (emitting
// "<span>.opened"/"<span>.closed" on the host channel — enter/exit semantics
// at tick-locked boundaries; m4 binds richer activation onto this primitive).
//
// All times are AUTHORED SECONDS. They tick-lock ONCE at instantiate via the
// pinned rounding rule (statechart.h time_to_tick): one IEEE double multiply
// `seconds * ticks_per_second`, then llround (nearest, ties away from zero).
// The runtime is pure integer arithmetic — no accumulation, no per-tick float
// math, so every keyframe tick is deterministic across platforms and runs
// (0.30 @ 60 Hz -> tick 18; span [0.40, 0.80] -> ticks 24..48).
//
// End modes: kFinish emits "<state>.finished" (chaining is an ordinary
// Transition pair on it; the loader canonicalizes `then:` sugar to exactly
// that pair). kLoop wraps `loop_count` total passes then finishes (0 = loop
// forever, never finished). kHold pins the playhead at the end, no finished.
// Interruption/history: the playhead resets on exit, or saves and resumes
// when the owning StateDesc declares `history: true` (spec 4.1 couples the
// substate-resume and playhead-resume semantics in the ONE history flag).

// One trigger-track keyframe: fire `event` on the host channel at `time`.
struct TriggerTrackDesc {
    double time = 0.0;  // seconds, 0 <= time <= duration
    base::Name event;   // required
    base::Json payload; // object payload; null = empty object
};

// One span track: a named interval, open at `begin`, closed at `end`.
struct SpanTrackDesc {
    base::Name name;    // unique per sheet; derives "<name>.opened"/"<name>.closed"
    double begin = 0.0; // seconds, 0 <= begin <= end <= duration
    double end = 0.0;
};

enum class SequenceEnd : std::uint8_t {
    kFinish = 0, // emit "<state>.finished" when the playhead reaches the end
    kLoop = 1,   // wrap loop_count total passes, then finish
    kHold = 2,   // pin the playhead at the end; no finished event
};

// The dope sheet a state may own. Plain aggregate, exactly what a YAML
// sequence block parses into (m0-yaml-loader-run). Track declaration order is
// the same-tick tie-break within each track kind; across kinds a tick fires
// triggers, then span openings, then span closings (A.1 phase 4, normative).
struct SequenceDesc {
    std::vector<TriggerTrackDesc> triggers;
    std::vector<SpanTrackDesc> spans;
    double duration = 0.0; // seconds, must round to >= 1 tick
    SequenceEnd end = SequenceEnd::kFinish;
    std::uint32_t loop_count = 0; // kLoop only: total passes; 0 = forever
};

// One component a state owns while active (m1-scene-format, spec 4.1
// "states owning component sets"): `{Name: {field: value, ...}}` in the
// SAME shape a scene entity's `components:` list uses, but the vocabulary
// here is open-ended (NavFollow, StaggerTimer, ... — TS components, none of
// them native). `fields` is opaque JSON, exactly like TriggerTrackDesc's
// payload above: this struct is pure data, never interpreted by the
// statechart runtime in this milestone (no component types exist to
// activate yet) — core/loader validates the NAME against a supplied
// component vocabulary and reports an unknown one as a Gap in lenient mode;
// this header only carries the shape.
struct StateComponentDesc {
    base::Name type;
    base::Json fields; // object; empty object for a bare "Name: {}" entry
};

struct StateDesc {
    base::Name name;
    std::vector<TransitionDesc> transitions; // declaration order (A.2 rule 2)
    std::vector<WatcherDesc> watchers;
    std::vector<StateComponentDesc> components; // components this state owns (spec 4.1)
    std::vector<StateDesc> substates;           // document order = sibling attach order
    base::Name initial;                         // required iff substates is non-empty
    bool history = false;                       // resume last active substate on re-entry
                                                // AND the saved sequence playhead
    std::optional<SequenceDesc> sequence;       // the state's dope sheet, if any
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
