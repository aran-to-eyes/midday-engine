// core/statechart/statechart.h — the statechart entity runtime
// (m0-statechart-core): every entity is a state machine by default (spec
// section 4.1). Machines instantiate as SUBTREES of real entities — machine
// root under the host, regions under the root, states under regions — so
// state active IS subtree active (core/hierarchy activation, D-BUILD-030) and
// the behavioral structure is visible in the hierarchy itself.
//
// Execution semantics are Appendix A, normative:
//   * TRANSITIONS RUN INLINE on events (A.2). Each machine subscribes ONE
//     entity-bound listener (its root's StatechartMachine row) to the host's
//     private channel plus the declared named channels; the listener filters
//     event names against its transition tables (the D-BUILD-046 shape). On
//     an event, regions evaluate in declaration order: an unmarked region
//     collects candidates (any-state rules first, then the ACTIVE CHAIN's
//     pairs outermost-first — document order), picks the winner by priority
//     then collection order, journals every loser as voided, and executes
//     exit chain then enter chain per A.2.1 — all on the same call stack.
//   * ONE TRANSITION PER REGION PER TICK. Regions stamp the tick they
//     transitioned (D-BUILD-054 — the stamp IS the mark, cleared by time
//     instead of by a tick-begin hook); on a later event this tick, every
//     name-matching pair region-wide journals as voided with reason
//     "region_already_transitioned" (the A.3 trace's stagger.hit record).
//     Cascades from hooks dispatch nested through the bus and share its
//     depth cap (kMaxCascadeDepth); the own-region stamp is the cycle
//     breaker (A.2 rule 5).
//   * A.2.1 HOOK ORDER, recursive. Exit of S: S's script onExit FIRST (the
//     brain orchestrates while its parts are still live), then its active
//     substate exits (recursively, same template), then components (script
//     components arrive with the bindings node), then S's subtree
//     DEACTIVATES, then playhead reset/save (sequences attach at
//     m0-sequences). Enter is the exact mirror: subtree activates,
//     components, substate enters (initial / history / forced path toward a
//     deep target), script onEnter LAST. Net observable order for a nested
//     chain: scripts fire outer->inner on exit and inner->outer on enter;
//     deactivation completes deepest-first (the no-zombie-hitbox rule).
//   * SEQUENCES (dope-sheet states, m0-sequences) advance in A.1 PHASE 4:
//     machines in tree order, sheets in state flatten order (region
//     declaration order then document order). Keyframe seconds tick-lock at
//     instantiate via time_to_tick (below) — the runtime is pure integer
//     arithmetic, playhead model documented at instance.h RtSheet. Per due
//     tick, items fire in timeline order (triggers, span openings, span
//     closings; declaration order within a kind): trigger tracks fire bus
//     events on the host channel (cause = the phase marker), span
//     boundaries journal sequence.span_open/span_close and trigger
//     "<span>.opened"/"<span>.closed" (cause = the span record). A playhead
//     reaching the end fires "<state>.finished" through finish_state
//     (kFinish), wraps and journals sequence.loop (kLoop, loop_count total
//     passes; 0 = forever), or pins silently (kHold). INTERRUPTION (A.2.1):
//     open spans close INSIDE the exit chain — after the state script's
//     onExit, before the active substate exits, in reverse open order (exit
//     step 2) — and the playhead resets, or saves under the owning state's
//     history flag (exit step 5); re-entry resumes the saved position with
//     covering spans re-opening inside the enter chain (enter step 3, the
//     exact mirror). Phase-4 cost model: one reused (tree-order, machine)
//     sort + O(1) per sheet + O(1) per due item — names and ticks are
//     precomputed at instantiate, so steady state does no hashing and
//     allocates nothing beyond the journal/bus payloads of items that
//     actually fire.
//   * `when:` WATCHERS run in A.1 phase 3 via a tick hook: compiled boolean
//     expressions over the machine environment, evaluated for ACTIVE owning
//     states in hierarchy tree order then declaration order. Edge semantics:
//     a watcher arms FALSE when its state enters, fires its event once on
//     observing true (so a condition already true at entry fires on the
//     first evaluation), stays silent while true, and re-arms on observing
//     false — state exit also re-arms. Fired events trigger on the host
//     channel with the phase marker as cause.
//   * onFixedUpdate fires from the A.1 update phase (machines in tree order,
//     regions in declaration order, active chain outermost-first — the
//     phase-5 order); onUpdate is the frame-side hook, driven by
//     run_update() from a frame loop that does not exist headless — no A.1
//     phase fires it (D-BUILD-055).
//
// EVERYTHING JOURNALS (record-before-effect, the bus discipline):
//   statechart.instantiate {machine, entity, root}
//   statechart.transition  {machine, entity, region, from, to, via}
//   statechart.voided      {machine, entity, region, event, source, target,
//                           priority, reason}   reason: "lost" |
//                           "region_already_transitioned"
//   statechart.hook        {machine, entity, region, state, hook[, peer]}
//   statechart.filter_fault{machine, entity, region, source, event, error}
//   sequence.span_open     {machine, entity, region, state, span, playhead,
//                           via}   via: "playhead" | "resume"
//   sequence.span_close    {machine, entity, region, state, span, playhead,
//                           via}   via: "playhead" | "exit"
//   sequence.loop          {machine, entity, region, state, pass}
// Enter/exit hook records cite the transition record; the transition cites
// the trigger; watcher-fired triggers cite the phase marker — the A.3 cause
// chain reconstructs mechanically. Enter/exit/transition/voided records are
// FLIGHT; update/fixed-update hook records are TRACE (per-tick volume — ids
// consumed either way, so FLIGHT bytes are invariant, D-BUILD-032).
//
// Allocation: the per-event transition path allocates NOTHING beyond journal
// records and their payloads (the inherent everything-journals class — same
// as the bus's per-trigger record). Candidate/chain scratch lives in
// per-cascade-depth frames sized once at construction; expression eval is
// noexcept and heap-free (core/expr contract).
//
// Lifecycle: one Statechart per World (registers the StatechartMachine /
// StatechartState component pools — boot path, aborts on a second).
// instantiate() is a BOOT/STRUCTURAL-PHASE operation: it spawns the subtree
// directly, flushes the structural queue to realize topology, then enters
// initial chains (A.1 phase 8 semantics; refused mid-iteration by the ECS
// lock). The Statechart must be destroyed before its World/Bus/TickLoop and
// never mid-tick (it detaches its phase hooks and subscriptions).

#pragma once

#include "core/base/error.h"
#include "core/base/name.h"
#include "core/bus/bus.h"
#include "core/ecs/entity.h"
#include "core/expr/value.h"
#include "core/journal/record.h"
#include "core/statechart/machine_desc.h"
#include "core/tick/tick_loop.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace midday::ecs {
class World;
}

namespace midday::hierarchy {
class Hierarchy;
}

namespace midday::journal {
class Writer;
}

namespace midday::statechart {

class Statechart;
struct MachineInstance; // internal runtime representation (instance.h)

using MachineId = std::uint32_t;
inline constexpr MachineId kInvalidMachine = 0xFFFFFFFFU;

// THE tick-locking rule (MILESTONE_0 item 19, pinned by sequence.tick_lock):
//   tick = llround(seconds * ticks_per_second)
// One correctly-rounded IEEE-754 double multiply, then round to the nearest
// integer with ties AWAY FROM ZERO (std::llround — independent of the FP
// rounding mode). Applied exactly once per keyframe at instantiate; no dt
// accumulation ever. KATs: 0.30 @ 60 -> 18, 0.40 @ 60 -> 24, 0.80 @ 60 -> 48,
// 1.2 @ 60 -> 72, tie 0.025 @ 60 (product exactly 1.5) -> 2, 0.30 @ 30 -> 9.
// Callers validate range/finiteness BEFORE converting (instantiate refuses
// non-finite or negative times with statechart.bad_sequence).
[[nodiscard]] std::int64_t time_to_tick(double seconds, std::uint32_t ticks_per_second);

// What every state hook invocation receives.
struct StateHookContext {
    MachineId machine = kInvalidMachine;
    ecs::EntityRef host;         // the entity owning the machine
    ecs::EntityRef state_entity; // the state's own hierarchy node
    base::Name region;
    base::Name state;
    // Transition peer: the `to` state for on_exit, the `from` state for
    // on_enter (empty at instantiate entry); empty for update flavors.
    base::Name peer;
    double dt = 0.0;             // update flavors only
    std::uint64_t record_id = 0; // journal id of THIS hook's record — THE
                                 // cause id for effects (bus triggers)
    std::uint64_t tick = 0;
};

// The per-state C++ hook interface (spec 4.1 lifecycle hooks — the state
// script's seat; TS state scripts bind onto this at the bindings node). The
// statechart stores the pointer and never owns it: hooks must have stable
// addresses and outlive the machine (or the Statechart).
class StateHooks {
public:
    StateHooks() = default;
    StateHooks(const StateHooks&) = default;
    StateHooks& operator=(const StateHooks&) = default;
    StateHooks(StateHooks&&) = default;
    StateHooks& operator=(StateHooks&&) = default;

    virtual void on_enter(Statechart& chart, const StateHookContext& context);
    virtual void on_exit(Statechart& chart, const StateHookContext& context);
    virtual void on_update(Statechart& chart, const StateHookContext& context);
    virtual void on_fixed_update(Statechart& chart, const StateHookContext& context);

protected:
    ~StateHooks() = default; // never deleted through this interface
};

// ECS-resident machine marker on the machine ROOT entity: the bus delivery
// target (entity-bound subscription, D-BUILD-048 — dormant root hears
// nothing, stale root auto-unsubscribes). Registered as "StatechartMachine".
struct MachineRoot {
    Statechart* system = nullptr;
    MachineId machine = kInvalidMachine;

    void on_event(bus::Bus& bus, const bus::EventView& event);
};

// ECS-resident state marker on every state entity: entity -> state identity
// for introspection and tooling. Registered as "StatechartState".
struct StateNode {
    base::Name machine;
    base::Name region;
    base::Name state;
    MachineId machine_id = kInvalidMachine;
    std::uint32_t state_index = 0; // index into the machine's state table
};

struct InstantiateResult {
    MachineId machine = kInvalidMachine;
    std::optional<base::Error> error; // engaged iff machine == kInvalidMachine
};

struct StatechartStats {
    std::uint64_t machines = 0;      // successful instantiations
    std::uint64_t transitions = 0;   // executed transitions
    std::uint64_t voided = 0;        // voided candidates journaled
    std::uint64_t watcher_fires = 0; // rising-edge watcher triggers
    std::uint64_t filter_faults = 0; // runtime eval faults (journaled, pair skipped)
    // Sequences (m0-sequences).
    std::uint64_t sequence_triggers = 0; // trigger-track keyframes fired
    std::uint64_t span_opens = 0;        // span openings (incl. resume re-opens)
    std::uint64_t span_closes = 0;       // span closings (incl. exit-chain closes)
    std::uint64_t sequence_loops = 0;    // playhead wraps journaled
    std::uint64_t sequence_finishes = 0; // "<state>.finished" emissions
};

class Statechart final : public tick::PhaseHook {
public:
    // All collaborators must outlive the Statechart. Registers the component
    // pools and attaches to the watchers + update phases (boot wiring).
    Statechart(ecs::World& world,
               hierarchy::Hierarchy& hierarchy,
               bus::Bus& bus,
               journal::Writer& journal,
               tick::TickLoop& loop);
    ~Statechart(); // detaches phase hooks + subscriptions (never mid-tick)

    Statechart(const Statechart&) = delete;
    Statechart& operator=(const Statechart&) = delete;
    Statechart(Statechart&&) = delete;
    Statechart& operator=(Statechart&&) = delete;

    // ---- instantiation (boot/structural phase; see header contract) -------
    // Validates the whole description first (nothing mutates on refusal),
    // builds the machine subtree under `host` (which must be alive and
    // hierarchy-adopted), flushes the structural queue, deactivates every
    // non-initial state, subscribes, journals, and enters the initial chains
    // (full A.2.1 enter order, hook records citing the instantiate record).
    InstantiateResult
    instantiate(const MachineDesc& desc, ecs::EntityRef host, std::uint64_t cause_id = 0);

    // ---- hooks (boot wiring; replaces any previous registration) ----------
    std::optional<base::Error>
    set_state_hooks(MachineId machine, base::Name region, base::Name state, StateHooks& hooks);

    // ---- expression environment binding ------------------------------------
    // Writes one declared slot; the value must carry the declared type
    // exactly ("statechart.var_type"). String values must outlive evals.
    std::optional<base::Error> set_var(MachineId machine, std::string_view name, expr::Value value);

    // ---- read-only introspection (spec 4.2: the universal query surface) --
    // True when `state` is anywhere in the region's active chain.
    [[nodiscard]] bool in_state(MachineId machine, base::Name region, base::Name state) const;
    // The region's top-level active state name (empty when unknown ids).
    [[nodiscard]] base::Name active_state(MachineId machine, base::Name region) const;
    [[nodiscard]] ecs::EntityRef machine_root(MachineId machine) const;
    [[nodiscard]] ecs::EntityRef
    state_entity(MachineId machine, base::Name region, base::Name state) const;

    // ---- state.finished (the sequences attachment point, spec 4.1) --------
    // Triggers "<state>.finished" on the host channel (payload {entity,
    // region, state} — the state.finished family schema, D-BUILD-056); the
    // state must be active. m0-sequences calls this when a playhead ends;
    // chaining is an ordinary transition pair on the event.
    std::optional<base::Error>
    finish_state(MachineId machine, base::Name region, base::Name state, std::uint64_t cause_id);

    // ---- frame-side update (no A.1 phase; see header comment) -------------
    void run_update(double dt, std::uint64_t cause_id);

    [[nodiscard]] const StatechartStats& stats() const { return stats_; }

    // tick::PhaseHook — watchers (A.1 phase 3), sequences (phase 4), and
    // update (phase 5).
    void on_phase(tick::TickLoop& loop, const tick::PhaseContext& context) override;

private:
    friend struct MachineRoot;

    // Per-cascade-depth scratch: sized once (bus depth is capped), reused
    // forever — the zero-allocation guarantee of the event path. Frames are
    // indexed by bus::Bus::cascade_depth(): dispatch runs at depth >= 1, so
    // frame 0 belongs to phase-level code and nesting levels never share.
    struct ScratchFrame {
        std::vector<std::uint32_t> candidates; // transition indices, decl order
        std::vector<std::uint32_t> enter_path; // region-root -> target
    };

    // transitions.cpp — the A.2 algorithm (inline on events).
    void on_machine_event(MachineId machine, const bus::EventView& event);
    void evaluate_region(MachineInstance& instance,
                         std::uint32_t region_index,
                         const bus::EventView& event,
                         ScratchFrame& frame);
    void void_marked_region(MachineInstance& instance,
                            std::uint32_t region_index,
                            const bus::EventView& event);
    bool filter_passes(MachineInstance& instance,
                       std::uint32_t transition_index,
                       const bus::EventView& event);
    void execute_transition(MachineInstance& instance,
                            std::uint32_t region_index,
                            std::uint32_t transition_index,
                            const bus::EventView& event,
                            ScratchFrame& frame);
    void exit_state(MachineInstance& instance,
                    std::uint32_t state_index,
                    base::Name to,
                    std::uint64_t cause_id);
    void enter_state(MachineInstance& instance,
                     std::uint32_t state_index,
                     const std::vector<std::uint32_t>& path,
                     std::uint32_t path_pos,
                     base::Name from,
                     std::uint64_t cause_id,
                     bool initial_entry);
    std::uint64_t journal_hook(MachineInstance& instance,
                               std::uint32_t state_index,
                               std::string_view hook,
                               base::Name peer,
                               std::uint64_t cause_id,
                               journal::Tier tier);
    void journal_voided(MachineInstance& instance,
                        std::uint32_t region_index,
                        std::uint32_t transition_index,
                        std::string_view reason,
                        std::uint64_t cause_id);
    void journal_fault(MachineInstance& instance,
                       base::Name region,
                       base::Name source,
                       base::Name event,
                       std::string_view code,
                       std::uint64_t cause_id);

    // watchers.cpp — A.1 phase 3.
    void run_watchers(const tick::PhaseContext& context);

    // sequences.cpp — A.1 phase 4 + the A.2.1 sequence steps + compilation.
    std::optional<base::Error> compile_sequence(const SequenceDesc& desc,
                                                std::uint32_t state_index,
                                                MachineInstance& staged,
                                                const std::string& origin) const;
    void run_sequences(const tick::PhaseContext& context);
    void advance_sheet(MachineInstance& instance,
                       std::uint32_t sheet_index,
                       const tick::PhaseContext& context);
    // Fires items due at or before the playhead, from the cursor, in
    // timeline order; stops when a cascade exits or re-enters the state.
    void
    fire_due_items(MachineInstance& instance, std::uint32_t sheet_index, std::uint64_t cause_id);
    // A.2.1 enter 3 (sequence half): playhead 0 (tick-0 items fire) or the
    // saved position (covering spans re-open). Called from enter_state.
    void sheet_start(MachineInstance& instance, std::uint32_t state_index, std::uint64_t cause_id);
    // A.2.1 exit 2: open spans close, reverse open order. From exit_state.
    void sheet_close_open_spans(MachineInstance& instance,
                                std::uint32_t state_index,
                                std::uint64_t cause_id);
    // A.2.1 exit 5: playhead resets, or saves under history. From exit_state.
    void sheet_settle_exit(MachineInstance& instance, std::uint32_t state_index);
    // Journals sequence.span_open/span_close, flips the flag, triggers the
    // "<span>.opened"/"<span>.closed" event (cause = the span record).
    void emit_span(MachineInstance& instance,
                   std::uint32_t sheet_index,
                   std::uint32_t span_index,
                   bool open,
                   std::string_view via,
                   std::uint64_t cause_id);

    // hooks.cpp — phase 5 fixed update + the frame-side flavor + shared
    // hook-invocation plumbing.
    void run_update_hooks(bool fixed, double dt, std::uint64_t cause_id);
    StateHookContext hook_context(MachineInstance& instance,
                                  std::uint32_t state_index,
                                  base::Name peer,
                                  double dt,
                                  std::uint64_t record_id) const;

    // instantiate.cpp
    std::optional<base::Error> validate_and_compile(const MachineDesc& desc,
                                                    MachineInstance& staged) const;
    std::optional<base::Error>
    build_subtree(MachineInstance& staged, ecs::EntityRef host, MachineId id);

    [[nodiscard]] MachineInstance* find_machine(MachineId machine) const;
    // Alive check with lazy retirement (host despawn cascades the subtree;
    // the bus auto-unsubscribes the stale root — D-BUILD-048).
    [[nodiscard]] bool machine_live(MachineInstance& instance);

    ecs::World* world_;
    hierarchy::Hierarchy* hierarchy_;
    bus::Bus* bus_;
    journal::Writer* journal_;
    tick::TickLoop* loop_;
    std::vector<std::unique_ptr<MachineInstance>> machines_;
    std::vector<ScratchFrame> scratch_; // indexed by bus cascade depth
    // Reused phase-order buffers: {sort key, machine id}. Watcher keys pack
    // (owning-state tree order << 32) | watcher index — tree order then
    // declaration order, the A.1 phase-3 order; machine keys are the root's
    // tree order (the phase-4 AND phase-5 scene order — the two phases never
    // overlap, so they share the buffer).
    std::vector<std::pair<std::uint64_t, std::uint32_t>> watcher_order_;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> machine_order_;
    StatechartStats stats_;
};

} // namespace midday::statechart
