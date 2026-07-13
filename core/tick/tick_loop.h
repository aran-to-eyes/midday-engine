// core/tick/tick_loop.h — the engine's heartbeat (m0-tick-loop): the fixed
// tick loop that runs Appendix A.1's nine phases, in order, every tick,
// forever. Every later system hangs off this loop; it stays SIMPLE and
// inspectable by design.
//
// The phase cycle (Appendix A.1, NORMATIVE — see core/tick/phase.h):
//   tick-begin · input · watchers · sequences · update · physics · post ·
//   structural-apply · tick-end
// Every phase journals ONE FLIGHT marker {kind:"tick.phase",
// payload:{phase}} before its body runs — the journal shows the heartbeat,
// and the tick.phases exit test asserts the exact 9-marker cycle per tick.
// The tick-begin marker is the tick's root record (cause 0); the other
// eight markers cite it, so a tick reads as one causality node.
//
// Phase hooks: subsystems attach to the five OPEN phases (watchers,
// sequences, update, physics, post) via add_hook — ordered registration,
// run in registration order, exactly the bus discipline. Hook wiring is a
// BOOT operation: add/remove mid-tick is a structured refusal
// ("tick.hook_locked"), which is what freezes hook vectors during the tick
// and makes index iteration re-entrancy-proof with zero snapshots.
// Transitions do NOT get a phase — they run inline on events, wherever
// events fire (Appendix A.2; the statechart node attaches to this loop, the
// loop only provides the phases).
//
// Engine-owned phases:
//   input            — drains the inject_input queue onto the bus, FIFO.
//                      Injected inputs are journaled ROOT records (cause 0):
//                      they enter from outside the sim. Inputs injected
//                      DURING a tick (by hooks/cascades) deliver next tick —
//                      the queue swap at phase entry is the cutoff.
//   structural-apply — World::flush_structural (queued spawns/despawns/
//                      reparents, queue order) + Hierarchy transform
//                      propagate. THE one deterministic mutation point
//                      (spec A.1 phase 8); tree order indices rebuild
//                      lazily off the flushed topology.
//   tick-end         — frame-packet capture + publish (core/tick/
//                      frame_packet.h seam), then journal flush on the
//                      configured cadence.
//
// Stepping: tick() advances exactly one fixed tick of dt = 1/ticks_per_
// second seconds (default 1/60 — the double is computed once from the
// integer config and never re-derived, so every dt consumer sees the
// identical bits). run_to_tick()/tick(n) are deterministic batch stepping;
// advance(elapsed) is the real-time accumulator with max-catch-up clamping
// (dropped time is counted, never silently lost). The loop NEVER reads a
// wall clock — real-time pacing feeds elapsed seconds in from outside
// (D-BUILD-013 discipline; headless first).
//
// Errors: no exceptions in the tick path. A poisoned journal refuses the
// tick ("tick.journal_refused") — unjournaled ticks do not exist, the bus
// rule lifted to the loop. Refused input triggers (e.g. payload_invalid)
// are counted and skipped: the bus already journaled the refusal, and one
// bad input must not stop the heartbeat.
//
// Allocation: the loop machinery allocates NOTHING per tick at steady
// state — hook iteration is index-based over frozen vectors, the input
// queue swap reuses capacity. The ONE exception is journal-inherent: each
// phase marker builds a one-field Json payload ({"phase": <name>}, a small
// string + one map node) because "everything journals" costs a record, and
// records own their payloads. Measured & accepted: 9 small payloads per
// tick, same class as every other journal write.
//
// Determinism (spec 4.3): every journal byte, every phase order, every
// flush point is a pure function of (config, operation script). Pinned by
// tick.determinism: two independently constructed loops driven by the same
// script produce byte-identical journal streams.

#pragma once

#include "core/base/error.h"
#include "core/base/json.h"
#include "core/base/name.h"
#include "core/bus/bus.h"
#include "core/tick/frame_packet.h"
#include "core/tick/phase.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
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

namespace midday::tick {

class TickLoop;

struct TickLoopConfig {
    // The fixed sim rate. dt = 1.0 / ticks_per_second, computed once at
    // construction (60 -> 0x1.1111111111111p-6 s, exactly, everywhere).
    std::uint32_t ticks_per_second = 60;
    // advance() runs at most this many ticks per call; surplus accumulated
    // time is DROPPED (counted in stats) — the spiral-of-death guard.
    std::uint32_t max_catchup_steps = 8;
    // tick-end journal flush cadence in ticks (crash durability at
    // deterministic points). 0 = never flush (fastest; close() still
    // finalizes). Flush points shape the compressed bytes, so this knob is
    // part of the byte-compare identity.
    std::uint32_t journal_flush_stride = 1;
};

// What every phase hook receives.
struct PhaseContext {
    Phase phase = Phase::kTickBegin;
    std::uint64_t tick = 0;            // current tick number (first tick = 1)
    double dt = 0.0;                   // the fixed dt, seconds
    std::uint64_t phase_record_id = 0; // journal id of THIS phase's marker —
                                       // the cause id for engine-initiated
                                       // effects in this phase
    std::uint64_t tick_record_id = 0;  // journal id of the tick-begin marker
};

// The phase subscriber interface (mirrors bus::EventListener): the loop
// stores the pointer and never owns it — a hook must have a stable address
// and must remove itself (between ticks) before it dies.
class PhaseHook {
public:
    PhaseHook() = default;
    PhaseHook(const PhaseHook&) = default;
    PhaseHook& operator=(const PhaseHook&) = default;
    PhaseHook(PhaseHook&&) = default;
    PhaseHook& operator=(PhaseHook&&) = default;

    virtual void on_phase(TickLoop& loop, const PhaseContext& context) = 0;

protected:
    ~PhaseHook() = default; // the loop never deletes through this interface
};

struct AdvanceResult {
    std::uint32_t ticks_run = 0;      // fixed ticks stepped by this call
    double alpha = 0.0;               // leftover accumulator / dt — the render
                                      // interpolation factor after this call
    std::optional<base::Error> error; // first tick error, if any (stepping stops)
};

struct TickStats {
    std::uint64_t ticks = 0;            // completed ticks
    std::uint64_t inputs_injected = 0;  // inject_input accepted
    std::uint64_t inputs_delivered = 0; // input-phase triggers the bus accepted
    std::uint64_t inputs_refused = 0;   // input-phase triggers the bus refused
                                        // (refusal journaled by the bus; loop continues)
    std::uint64_t catchup_clamps = 0;   // advance() calls that hit the clamp
    double dropped_seconds = 0.0;       // sim time discarded by clamping
};

class TickLoop {
public:
    // All four collaborators must outlive the loop. The canonical
    // composition: THE World, its Hierarchy, THE bus, THE flight recorder —
    // one loop per sim.
    TickLoop(ecs::World& world,
             hierarchy::Hierarchy& hierarchy,
             bus::Bus& bus,
             journal::Writer& journal,
             TickLoopConfig config = {});

    TickLoop(const TickLoop&) = delete;
    TickLoop& operator=(const TickLoop&) = delete;
    TickLoop(TickLoop&&) = delete;
    TickLoop& operator=(TickLoop&&) = delete;
    ~TickLoop() = default;

    // ---- phase hooks (boot wiring; refused mid-tick) -----------------------
    // Ordered registration per phase; open phases only ("tick.phase_reserved"
    // otherwise). Duplicate add -> "tick.duplicate_hook"; absent remove ->
    // "tick.hook_absent"; either mid-tick -> "tick.hook_locked".
    std::optional<base::Error> add_hook(Phase phase, PhaseHook& hook);
    std::optional<base::Error> remove_hook(Phase phase, PhaseHook& hook);
    [[nodiscard]] std::uint32_t hook_count(Phase phase) const;

    // ---- the ONE structural-apply extension slot (m1-prefab-spawn), now
    // TWO-PHASE (M2 0B, D4) -------------------------------------------------
    // structural-apply (phase 8) is engine-owned — no add_hook, ever
    // (core/tick/phase.h). This is the single, well-known socket a runtime
    // spawn/despawn extension installs at boot, mirroring ecs::World's own
    // reparent_handler_/despawn_observer_ pattern — one owner, two calls
    // bracketing the flush (run_structural_apply, tick_loop.cpp):
    //
    //   preparer(tick, phase_record_id)  — BEFORE World::flush_structural.
    //       The despawn-linger half (D4): exit-chains-before-removal at the
    //       exact ceiling tick is impossible post-flush-only, so due
    //       entities run their full statechart exit chains + component
    //       onExit hooks HERE, while they are provably still alive, and
    //       only then queue their despawns into the very flush that
    //       follows. Unset (the default) is a no-op — every pre-0B
    //       composition, including 0A's route-only realizer wiring,
    //       compiles and behaves byte-identically.
    //   realizer(phase_record_id)        — AFTER the flush, BEFORE
    //       Hierarchy::propagate: realizes queued prefab spawns (adopt,
    //       machine instantiate — the enter chain, A.1 phase 8) and fires
    //       despawn lifecycle events, now that the entities it reserved are
    //       provably alive/dead; a realized prefab's local transform lands
    //       in this tick's world-matrix settle.
    //
    // `phase_record_id` is the structural-apply phase marker's journal id,
    // the cause id for every effect either half journals/triggers this tick
    // (the PhaseContext::phase_record_id convention). A returned error halts
    // the tick exactly like any other phase failure. Anything captured by
    // reference must outlive the loop or be cleared before it dies — a
    // composition that wires BOTH halves must clear BOTH in its teardown
    // (the RunSim ~dtor discipline).
    using StructuralRealizer = std::function<std::optional<base::Error>(std::uint64_t)>;
    // (tick, phase_record_id) — the spec-literal prepare shape (D4). The
    // realizer keeps its m1 one-argument shape: existing installers stay
    // source-compatible, and its tick is observable via the bus stamp.
    using StructuralPreparer =
        std::function<std::optional<base::Error>(std::uint64_t, std::uint64_t)>;

    void set_structural_realizer(StructuralRealizer realizer) { realizer_ = std::move(realizer); }

    void set_structural_preparer(StructuralPreparer preparer) { preparer_ = std::move(preparer); }

    // ---- input feed (the headless injection point; devices arrive later) --
    // Queues one input event for the NEXT input phase (FIFO). Journals
    // nothing yet — the bus journals the trigger, as a root record, when the
    // phase drains it. Null key refused now ("tick.null_key"): a doomed
    // injection should fail at the caller, not mid-tick.
    std::optional<base::Error>
    inject_input(bus::EventKey key, base::Name event, base::Json payload);

    [[nodiscard]] std::size_t pending_input_count() const { return input_queue_.size(); }

    // ---- stepping ----------------------------------------------------------
    // One fixed tick: the nine phases, in order. Re-entry from a hook is
    // refused ("tick.reentrant").
    std::optional<base::Error> tick();

    // Exactly `count` ticks; stops at the first error.
    std::optional<base::Error> tick(std::uint64_t count);

    // Ticks until current_tick() == target (no-op if already past).
    std::optional<base::Error> run_to_tick(std::uint64_t target);

    // Real-time mode: add elapsed seconds to the accumulator, clamp to
    // max_catchup_steps * dt (dropping the surplus, counted), then step
    // whole ticks. Negative elapsed is treated as 0. The caller supplies
    // elapsed time — the loop never reads a clock.
    AdvanceResult advance(double elapsed_seconds);

    // ---- observation -------------------------------------------------------
    [[nodiscard]] std::uint64_t current_tick() const { return tick_; } // 0 before the first

    [[nodiscard]] double dt_seconds() const { return dt_; }

    // The configured fixed rate — the tick-locking base for sequence
    // keyframes (statechart::time_to_tick) and every other seconds->tick
    // conversion; never re-derive it from dt.
    [[nodiscard]] std::uint32_t ticks_per_second() const { return config_.ticks_per_second; }

    [[nodiscard]] bool ticking() const { return in_tick_; }

    [[nodiscard]] double interpolation_alpha() const { return accumulator_ / dt_; }

    [[nodiscard]] const TickStats& stats() const { return stats_; }

    // The render side of the extraction seam (read the front packet).
    [[nodiscard]] const FramePacketBuffer& frame_packets() const { return packets_; }

private:
    struct InjectedInput {
        bus::EventKey key;
        base::Name event;
        base::Json payload;
    };

    // Writes the phase's tick.phase marker; returns its record id, 0 when
    // the journal refused (sticky error -> the tick refuses).
    std::uint64_t phase_marker(Phase phase, std::uint64_t cause_id);

    // Phase bodies (tick_loop.cpp).
    std::optional<base::Error> run_input_phase();
    std::optional<base::Error> run_structural_apply(std::uint64_t phase_record_id);
    std::optional<base::Error> run_tick_end();
    void run_hooks(Phase phase, std::uint64_t phase_record_id, std::uint64_t tick_record_id);

    ecs::World* world_;
    hierarchy::Hierarchy* hierarchy_;
    bus::Bus* bus_;
    journal::Writer* journal_;
    TickLoopConfig config_;
    double dt_;

    std::array<std::vector<PhaseHook*>, kPhaseCount> hooks_; // open phases only
    std::vector<InjectedInput> input_queue_;                 // FIFO, next tick's input
    std::vector<InjectedInput> input_drain_;                 // swap target (capacity reused)
    StructuralPreparer preparer_;                            // pre-flush half (D4)
    StructuralRealizer realizer_;                            // see set_structural_realizer
    FramePacketBuffer packets_;
    TickStats stats_;
    std::uint64_t tick_ = 0;
    double accumulator_ = 0.0;
    bool in_tick_ = false;
};

} // namespace midday::tick
