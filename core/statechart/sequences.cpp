// core/statechart/sequences.cpp — dope-sheet sequence states (m0-sequences,
// spec 4.1): compilation (tick-locking at instantiate), the A.1 phase-4
// advance, and the A.2.1 enter/exit chain steps sequences own.
//
// TICK-LOCKING (the pinned rule, statechart.h): every authored second value
// converts ONCE at instantiate — llround(seconds * ticks_per_second) — and
// the runtime is pure integer arithmetic. No dt accumulation, no per-tick
// float math: a keyframe's tick is a deterministic function of (authored
// seconds, tick rate) on every platform.
//
// PLAYHEAD MODEL (instance.h RtSheet): local tick L fires at global tick
// E + L (E = the entry tick) — the playhead advances by exactly one at each
// sequences phase the owning state is active and non-dormant for; the entry
// tick's own phase 4 is skipped (entry already positioned the sheet inside
// the enter chain), and dormancy pauses the sheet. Items due at a position
// fire in timeline order: (tick, kind: triggers/openings/closings,
// declaration) — the precomputed sort order of the item table.
//
// COST MODEL (phase 4 is exactly the cost of advancing N active sheets):
// one reused (tree-order, machine) sort over machines that own sheets, an
// O(1) active/done/entered check per sheet, and O(1) per DUE item — event
// names interned and ticks precomputed at instantiate, so steady state does
// no hashing and allocates nothing beyond the journal/bus payloads of items
// that actually fire (the journal-inherent class, D-BUILD-050).

#include "core/base/json.h"
#include "core/ecs/world.h"
#include "core/hierarchy/hierarchy.h"
#include "core/journal/writer.h"
#include "core/statechart/instance.h"
#include "core/statechart/statechart.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace midday::statechart {

std::int64_t time_to_tick(double seconds, std::uint32_t ticks_per_second) {
    // One correctly-rounded IEEE-754 multiply, then nearest-integer with
    // ties away from zero. std::llround is independent of the ambient FP
    // rounding mode — the same bits everywhere (deterministic-FP contract).
    return std::llround(seconds * static_cast<double>(ticks_per_second));
}

namespace {

base::Error sheet_error(std::string_view message, const std::string& origin) {
    base::Error error;
    error.code = "statechart.bad_sequence";
    error.message = std::string(message);
    error.details.set("where", origin);
    return error;
}

// A keyframe time is legal when finite, non-negative, and on the sheet.
// llround is monotone, so the seconds-domain bound implies the tick-domain
// bound (tick <= duration_ticks) — one rule, no double-checking.
bool bad_time(double seconds, double duration) {
    return !std::isfinite(seconds) || seconds < 0.0 || seconds > duration;
}

} // namespace

// ---- compilation (called from validate_and_compile, atomic refusal) --------

std::optional<base::Error> Statechart::compile_sequence(const SequenceDesc& desc,
                                                        std::uint32_t state_index,
                                                        MachineInstance& staged,
                                                        const std::string& origin) const {
    if (!std::isfinite(desc.duration) || desc.duration <= 0.0)
        return sheet_error("sequence duration must be positive seconds", origin);
    const std::uint32_t tps = loop_->ticks_per_second();
    const std::int64_t duration_ticks = time_to_tick(desc.duration, tps);
    if (duration_ticks < 1)
        return sheet_error("sequence duration rounds to zero ticks", origin);
    if (duration_ticks > static_cast<std::int64_t>(0xFFFFFFFFU))
        return sheet_error("sequence duration exceeds the tick range", origin);
    if (desc.end != SequenceEnd::kLoop && desc.loop_count != 0)
        return sheet_error("loop_count applies only to end: loop", origin);

    RtSheet sheet;
    sheet.state = state_index;
    sheet.duration = static_cast<std::uint32_t>(duration_ticks);
    sheet.end = desc.end;
    sheet.loop_count = desc.loop_count;
    sheet.first_span = static_cast<std::uint32_t>(staged.sheet_spans.size());
    sheet.first_item = static_cast<std::uint32_t>(staged.sheet_items.size());

    for (const SpanTrackDesc& span_desc : desc.spans) {
        if (span_desc.name.empty())
            return sheet_error("span name must be non-empty", origin);
        for (std::size_t prior = sheet.first_span; prior < staged.sheet_spans.size(); ++prior) {
            if (staged.sheet_spans[prior].name == span_desc.name) {
                base::Error error = sheet_error("span name declared twice in the sheet", origin);
                error.code = "statechart.duplicate_span";
                return error;
            }
        }
        if (bad_time(span_desc.begin, desc.duration) || bad_time(span_desc.end, desc.duration) ||
            span_desc.begin > span_desc.end)
            return sheet_error("span needs 0 <= begin <= end <= duration", origin);
        RtSpan span;
        span.name = span_desc.name;
        span.opened_event = base::Name(std::string(span_desc.name.view()) + ".opened");
        span.closed_event = base::Name(std::string(span_desc.name.view()) + ".closed");
        span.begin_tick = static_cast<std::uint32_t>(time_to_tick(span_desc.begin, tps));
        span.end_tick = static_cast<std::uint32_t>(time_to_tick(span_desc.end, tps));
        staged.sheet_spans.push_back(span);
    }
    for (const TriggerTrackDesc& trigger_desc : desc.triggers) {
        if (trigger_desc.event.empty())
            return sheet_error("trigger needs an event name", origin);
        if (!trigger_desc.payload.is_null() && !trigger_desc.payload.is_object())
            return sheet_error("trigger payload must be an object (or null)", origin);
        if (bad_time(trigger_desc.time, desc.duration))
            return sheet_error("trigger needs 0 <= time <= duration", origin);
        RtSheetItem item;
        item.tick = static_cast<std::uint32_t>(time_to_tick(trigger_desc.time, tps));
        item.kind = SheetItemKind::kTrigger;
        item.ref = static_cast<std::uint32_t>(staged.sheet_triggers.size());
        staged.sheet_items.push_back(item);
        RtTrigger trigger;
        trigger.event = trigger_desc.event;
        trigger.payload =
            trigger_desc.payload.is_object() ? trigger_desc.payload : base::Json::object();
        staged.sheet_triggers.push_back(std::move(trigger));
    }
    sheet.span_count = static_cast<std::uint32_t>(staged.sheet_spans.size()) - sheet.first_span;
    for (std::uint32_t s = 0; s < sheet.span_count; ++s) {
        const RtSpan& span = staged.sheet_spans[sheet.first_span + s];
        staged.sheet_items.push_back(
            {span.begin_tick, SheetItemKind::kSpanOpen, sheet.first_span + s});
        staged.sheet_items.push_back(
            {span.end_tick, SheetItemKind::kSpanClose, sheet.first_span + s});
    }
    sheet.item_count = static_cast<std::uint32_t>(staged.sheet_items.size()) - sheet.first_item;

    // Timeline order, precomputed: (tick, kind — triggers, openings,
    // closings) with declaration order preserved within a kind (stable).
    std::stable_sort(staged.sheet_items.begin() + sheet.first_item,
                     staged.sheet_items.end(),
                     [](const RtSheetItem& a, const RtSheetItem& b) {
                         if (a.tick != b.tick)
                             return a.tick < b.tick;
                         return static_cast<std::uint8_t>(a.kind) <
                                static_cast<std::uint8_t>(b.kind);
                     });

    staged.states[state_index].sheet = static_cast<std::uint32_t>(staged.sheets.size());
    staged.sheets.push_back(sheet);
    return std::nullopt;
}

// ---- A.1 phase 4 ------------------------------------------------------------

void Statechart::run_sequences(const tick::PhaseContext& context) {
    machine_order_.clear(); // shared with phase 5 (the phases never overlap)
    for (MachineId m = 0; m < machines_.size(); ++m) {
        MachineInstance& instance = *machines_[m];
        if (instance.sheets.empty() || !machine_live(instance))
            continue;
        if (hierarchy_->is_dormant(instance.root))
            continue; // dormant sheets pause: no phase experienced (spec 4.1)
        machine_order_.emplace_back(hierarchy_->order_index(instance.root).value_or(0xFFFFFFFFU),
                                    m);
    }
    std::ranges::sort(machine_order_);

    for (const auto& [order, m] : machine_order_) {
        MachineInstance& instance = *machines_[m];
        // Re-check: an earlier sheet's cascade may have retired or
        // deactivated this machine mid-phase.
        if (!machine_live(instance) || hierarchy_->is_dormant(instance.root))
            continue;
        // Sheets advance in state flatten order — region declaration order,
        // then document order (A.1 phase 4).
        for (std::uint32_t s = 0; s < instance.sheets.size(); ++s)
            advance_sheet(instance, s, context);
    }
}

void Statechart::advance_sheet(MachineInstance& instance,
                               std::uint32_t sheet_index,
                               const tick::PhaseContext& context) {
    RtSheet& sheet = instance.sheets[sheet_index];
    const RtState& state = instance.states[sheet.state];
    if (!state.active || sheet.done)
        return;
    if (sheet.entered_tick == context.tick)
        return;          // entered this tick: entry already positioned the sheet
    sheet.playhead += 1; // exactly one local tick per experienced phase
    fire_due_items(instance, sheet_index, context.phase_record_id);
    // A cascade may have exited the state (its exit chain settled the
    // sheet) or exited AND re-entered it (entry owns the runtime fields).
    if (!state.active || sheet.done || sheet.entered_tick == context.tick)
        return;
    if (sheet.playhead < static_cast<std::int64_t>(sheet.duration))
        return;

    // The playhead reached the end (== duration exactly: one-step advances).
    if (sheet.end == SequenceEnd::kLoop &&
        (sheet.loop_count == 0 || sheet.pass + 1 < sheet.loop_count)) {
        sheet.pass += 1;
        stats_.sequence_loops += 1;
        base::Json payload = base::Json::object();
        payload.set("machine", instance.name.view());
        payload.set("entity", entity_form(instance.host));
        payload.set("region", instance.regions[state.region].name.view());
        payload.set("state", state.name.view());
        payload.set("pass", static_cast<std::int64_t>(sheet.pass));
        journal_->record(bus_->tick(),
                         journal::Tier::Flight,
                         "sequence.loop",
                         context.phase_record_id,
                         std::move(payload));
        // The wrap tick IS local 0 of the next pass (position duration ==
        // position 0): its tick-0 items fire now, after the end items.
        sheet.playhead = 0;
        sheet.cursor = sheet.first_item;
        fire_due_items(instance, sheet_index, context.phase_record_id);
        return;
    }

    // kFinish / final loop pass emit finished; kHold pins silently.
    // Bookkeeping FIRST: the finished cascade may exit and re-enter this
    // very state, and entry must own the sheet state from then on.
    sheet.done = true;
    if (sheet.end == SequenceEnd::kHold)
        return;
    stats_.sequence_finishes += 1;
    // The statechart emission path (spec 4.1); a refused trigger already
    // journaled its refusal at the bus — the phase carries on.
    (void)finish_state(
        instance.id, instance.regions[state.region].name, state.name, context.phase_record_id);
}

void Statechart::fire_due_items(MachineInstance& instance,
                                std::uint32_t sheet_index,
                                std::uint64_t cause_id) {
    RtSheet& sheet = instance.sheets[sheet_index];
    const RtState& state = instance.states[sheet.state];
    const std::uint32_t items_end = sheet.first_item + sheet.item_count;
    const std::uint32_t epoch = sheet.epoch;
    while (sheet.cursor < items_end &&
           static_cast<std::int64_t>(instance.sheet_items[sheet.cursor].tick) <= sheet.playhead) {
        const RtSheetItem item = instance.sheet_items[sheet.cursor];
        sheet.cursor += 1; // consumed BEFORE effects: an interruption from
                           // the item's own cascade treats it as fired
        switch (item.kind) {
        case SheetItemKind::kTrigger: {
            const RtTrigger& trigger = instance.sheet_triggers[item.ref];
            stats_.sequence_triggers += 1;
            // Cause = the phase marker (engine-initiated, D-BUILD-050) or
            // the enter-chain record at entry. Refusals journal at the bus.
            (void)bus_->trigger(
                bus::EventKey::entity(instance.host), trigger.event, trigger.payload, cause_id);
            break;
        }
        case SheetItemKind::kSpanOpen:
            emit_span(instance, sheet_index, item.ref, true, "playhead", cause_id);
            break;
        case SheetItemKind::kSpanClose:
            // An interruption can only close spans after this batch broke
            // (checked below), so a due close normally sees open == true;
            // the guard keeps the invariant defensive, never double-closing.
            if (instance.sheet_spans[item.ref].open)
                emit_span(instance, sheet_index, item.ref, false, "playhead", cause_id);
            break;
        }
        // The item's cascade may have exited the state (spans closed in ITS
        // exit chain) or re-entered it (epoch bumped): the batch is stale.
        if (!state.active || sheet.epoch != epoch)
            break;
    }
}

// ---- the A.2.1 chain steps (called from transitions.cpp) --------------------

void Statechart::sheet_start(MachineInstance& instance,
                             std::uint32_t state_index,
                             std::uint64_t cause_id) {
    const RtState& state = instance.states[state_index];
    if (state.sheet == kInvalidIndex)
        return;
    RtSheet& sheet = instance.sheets[state.sheet];
    sheet.epoch += 1; // any in-flight batch of a prior activation is stale
    sheet.entered_tick = bus_->tick();
    const std::uint32_t items_end = sheet.first_item + sheet.item_count;
    if (state.history && sheet.saved) {
        // Resume (A.2.1 enter 3, history): the saved position is restored;
        // items at or before it fired before the interruption. Spans
        // covering the position re-open HERE, inside the enter chain — the
        // exact mirror of exit step 2 — in open order (forward item order).
        sheet.playhead = sheet.saved_playhead;
        sheet.pass = sheet.saved_pass;
        sheet.done = sheet.saved_done;
        sheet.saved = false; // consumed; the next exit re-saves
        std::uint32_t cursor = sheet.first_item;
        while (cursor < items_end &&
               static_cast<std::int64_t>(instance.sheet_items[cursor].tick) <= sheet.playhead)
            cursor += 1;
        sheet.cursor = cursor;
        for (std::uint32_t i = sheet.first_item; i < items_end; ++i) {
            const RtSheetItem item = instance.sheet_items[i];
            if (item.kind != SheetItemKind::kSpanOpen)
                continue;
            const RtSpan& span = instance.sheet_spans[item.ref];
            if (static_cast<std::int64_t>(span.begin_tick) <= sheet.playhead &&
                sheet.playhead < static_cast<std::int64_t>(span.end_tick))
                emit_span(instance, state.sheet, item.ref, true, "resume", cause_id);
        }
        return;
    }
    // Fresh start (A.2.1 enter 3: "sequence playhead starts at 0"): items
    // at local tick 0 fire now, inside the enter chain.
    sheet.playhead = 0;
    sheet.pass = 0;
    sheet.done = false;
    sheet.saved = false;
    sheet.cursor = sheet.first_item;
    fire_due_items(instance, state.sheet, cause_id);
}

void Statechart::sheet_close_open_spans(MachineInstance& instance,
                                        std::uint32_t state_index,
                                        std::uint64_t cause_id) {
    const RtState& state = instance.states[state_index];
    if (state.sheet == kInvalidIndex)
        return;
    const RtSheet& sheet = instance.sheets[state.sheet];
    // Reverse item order over the open items IS reverse open order (A.2.1
    // exit 2: deepest/latest first). Cause = the transition record.
    for (std::uint32_t i = sheet.first_item + sheet.item_count; i > sheet.first_item; --i) {
        const RtSheetItem item = instance.sheet_items[i - 1];
        if (item.kind == SheetItemKind::kSpanOpen && instance.sheet_spans[item.ref].open)
            emit_span(instance, state.sheet, item.ref, false, "exit", cause_id);
    }
}

void Statechart::sheet_settle_exit(MachineInstance& instance, std::uint32_t state_index) {
    const RtState& state = instance.states[state_index];
    if (state.sheet == kInvalidIndex)
        return;
    RtSheet& sheet = instance.sheets[state.sheet];
    if (state.history) { // A.2.1 exit 5: save under history...
        sheet.saved = true;
        sheet.saved_playhead = sheet.playhead;
        sheet.saved_pass = sheet.pass;
        sheet.saved_done = sheet.done;
    }
    // ...or reset — which happens at the next entry (sheet_start owns the
    // runtime fields); bumping the epoch stales any in-flight batch now.
    sheet.epoch += 1;
}

void Statechart::emit_span(MachineInstance& instance,
                           std::uint32_t sheet_index,
                           std::uint32_t span_index,
                           bool open,
                           std::string_view via,
                           std::uint64_t cause_id) {
    RtSheet& sheet = instance.sheets[sheet_index];
    const RtState& state = instance.states[sheet.state];
    RtSpan& span = instance.sheet_spans[span_index];
    base::Json payload = base::Json::object();
    payload.set("machine", instance.name.view());
    payload.set("entity", entity_form(instance.host));
    payload.set("region", instance.regions[state.region].name.view());
    payload.set("state", state.name.view());
    payload.set("span", span.name.view());
    payload.set("playhead", sheet.playhead);
    payload.set("via", via);
    const std::uint64_t record_id =
        journal_->record(bus_->tick(),
                         journal::Tier::Flight,
                         open ? "sequence.span_open" : "sequence.span_close",
                         cause_id,
                         std::move(payload));
    span.open = open;
    if (open)
        stats_.span_opens += 1;
    else
        stats_.span_closes += 1;
    // The boundary event (record before effect: it cites the span record).
    // Payload follows the state.finished family shape (D-BUILD-056).
    base::Json event_payload = base::Json::object();
    event_payload.set("entity", static_cast<std::int64_t>(instance.host.to_bits()));
    event_payload.set("region", instance.regions[state.region].name.view());
    event_payload.set("state", state.name.view());
    event_payload.set("span", span.name.view());
    (void)bus_->trigger(bus::EventKey::entity(instance.host),
                        open ? span.opened_event : span.closed_event,
                        event_payload,
                        record_id);
}

} // namespace midday::statechart
