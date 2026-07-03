// core/statechart/instance.h — the compiled runtime form of one machine
// (INTERNAL to core/statechart: included by the .cpps and by nothing else).
//
// Layout is flat and slice-addressed, sized once at instantiate and never
// reshaped: states in document order region by region (a region's states are
// one contiguous slice), transitions in region declaration order (each
// region's any-state rules FIRST, then its states' pairs in document order —
// so one contiguous [any_begin, transitions_end) range per region IS the A.2
// declaration order for both candidate collection and marked-region void
// scans), watchers in the same document order. The per-event path walks
// these arrays with zero hashing and zero allocation.

#pragma once

#include "core/base/json.h"
#include "core/base/name.h"
#include "core/bus/bus.h"
#include "core/ecs/entity.h"
#include "core/expr/env.h"
#include "core/expr/program.h"
#include "core/expr/value.h"
#include "core/statechart/statechart.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace midday::statechart {

inline constexpr std::uint32_t kInvalidIndex = 0xFFFFFFFFU;
// Region transition stamp before any transition (tick numbering starts at 1,
// and a pre-loop bus tick of 0 must still mark cleanly).
inline constexpr std::uint64_t kNeverTicked = 0xFFFFFFFFFFFFFFFFULL;

struct RtTransition {
    base::Name event;
    base::Name source;                    // owning state name; empty = any-state rule
    std::uint32_t target = kInvalidIndex; // state index in the machine table
    std::int32_t priority = 0;
    std::optional<expr::Program> filter; // nullopt = unconditional
};

struct RtWatcher {
    expr::Program program;
    base::Name event;                    // fired on the rising edge
    std::uint32_t state = kInvalidIndex; // owning state index
    bool armed_value = false;            // last observed value (false = armed)
};

// One trigger-track keyframe, tick-locked (statechart.h time_to_tick).
struct RtTrigger {
    base::Name event;
    base::Json payload; // always an object (null normalized at instantiate)
};

// One span track, tick-locked; opened/closed event names interned once.
struct RtSpan {
    base::Name name;
    base::Name opened_event; // "<name>.opened"
    base::Name closed_event; // "<name>.closed"
    std::uint32_t begin_tick = 0;
    std::uint32_t end_tick = 0;
    bool open = false; // runtime: open fired, close not yet
};

enum class SheetItemKind : std::uint8_t {
    kTrigger = 0,   // same-tick batch order (A.1 phase 4, normative):
    kSpanOpen = 1,  // triggers, then span openings, then span closings;
    kSpanClose = 2, // within a kind, track declaration order.
};

// One due-able timeline item. Per sheet, items sort by (tick, kind,
// declaration) at instantiate — the item slice IS the fire order, and the
// reverse of its kSpanOpen entries IS reverse span-open order (the A.2.1
// exit-2 interruption order). The per-tick path walks this array with zero
// hashing and zero allocation.
struct RtSheetItem {
    std::uint32_t tick = 0; // local sheet tick
    SheetItemKind kind = SheetItemKind::kTrigger;
    std::uint32_t ref = 0; // index into sheet_triggers / sheet_spans
};

// The compiled dope sheet of one state (spec 4.1 sequences). PLAYHEAD MODEL:
// local tick L fires at global tick E + L (E = the tick the state entered) —
// the playhead advances by exactly one at each sequences phase the owning
// state is active and non-dormant for; entry initializes it (0, or the saved
// position under history) inside the enter chain, where items at exactly the
// starting tick fire (fresh) or covering spans re-open (resume); the entry
// tick's own phase 4 is skipped (entered_tick). Dormancy pauses the sheet.
struct RtSheet {
    std::uint32_t state = kInvalidIndex; // owning state index
    std::uint32_t duration = 0;          // sheet length in ticks (>= 1)
    SequenceEnd end = SequenceEnd::kFinish;
    std::uint32_t loop_count = 0; // kLoop: total passes; 0 = forever
    // Slices (machine tables).
    std::uint32_t first_item = 0;
    std::uint32_t item_count = 0;
    std::uint32_t first_span = 0;
    std::uint32_t span_count = 0;
    // Runtime.
    std::int64_t playhead = 0;      // local tick last advanced to
    std::uint32_t cursor = 0;       // absolute index of the next unfired item
    std::uint32_t pass = 0;         // completed loop passes
    std::uint32_t epoch = 0;        // bumped on enter/exit: stops stale batches
    std::uint64_t entered_tick = 0; // the tick the owning state entered
    bool done = false;              // reached the end (finished emitted / holding)
    // History save (A.2.1 exit 5, owning state's history flag).
    bool saved = false;
    bool saved_done = false;
    std::int64_t saved_playhead = 0;
    std::uint32_t saved_pass = 0;
};

struct RtState {
    base::Name name;
    base::Name finished_event; // "<name>.finished", interned at instantiate
    ecs::EntityRef entity;
    std::uint32_t region = 0;
    std::uint32_t parent = kInvalidIndex;        // parent state (kInvalid = top-level)
    std::uint32_t initial_child = kInvalidIndex; // kInvalid = leaf
    std::uint32_t sheet = kInvalidIndex;         // this state's RtSheet, if any
    bool history = false;
    // Transition/watcher slices (into the machine tables).
    std::uint32_t first_transition = 0;
    std::uint32_t transition_count = 0;
    std::uint32_t first_watcher = 0;
    std::uint32_t watcher_count = 0;
    // Runtime.
    bool active = false;
    std::uint32_t active_child = kInvalidIndex;
    std::uint32_t history_child = kInvalidIndex; // last active child (history)
    StateHooks* hooks = nullptr;
};

struct RtRegion {
    base::Name name;
    ecs::EntityRef entity;
    std::uint32_t initial = kInvalidIndex; // top-level state index
    bool history = false;
    std::uint32_t history_state = kInvalidIndex;
    // Slices.
    std::uint32_t first_state = 0; // contiguous state slice (document order)
    std::uint32_t state_count = 0;
    std::uint32_t first_any = 0; // any-state rules; the region's transitions
    std::uint32_t any_count = 0; // run [first_any, transitions_end)
    std::uint32_t transitions_end = 0;
    // Runtime.
    std::uint32_t active = kInvalidIndex;          // top-level active state
    std::uint64_t transition_stamp = kNeverTicked; // tick of the last transition
};

struct MachineInstance {
    base::Name name;
    MachineId id = kInvalidMachine;
    ecs::EntityRef host;
    ecs::EntityRef root;
    expr::EnvSpec env;              // slot vocabulary (find() for set_var)
    std::vector<expr::Value> slots; // bound values, slot order
    std::vector<RtRegion> regions;
    std::vector<RtState> states;
    std::vector<RtTransition> transitions;
    std::vector<RtWatcher> watchers;
    std::vector<RtSheet> sheets; // state flatten order = A.1 phase-4 order
    std::vector<RtSheetItem> sheet_items;
    std::vector<RtTrigger> sheet_triggers;
    std::vector<RtSpan> sheet_spans;
    std::vector<bus::EventKey> keys; // subscribed channels (unsubscribe list)
    bool retired = false;            // host died; lazily detached
};

// The type's zero value — what unbound environment slots hold.
expr::Value zero_value(expr::ValueType type);

// Journal spelling of an entity handle, the bus's diagnostic form:
// "entity:<index>#<generation>".
std::string entity_form(ecs::EntityRef ref);

} // namespace midday::statechart
