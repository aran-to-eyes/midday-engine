// core/tick/tick_loop.cpp — the nine phases, in order, every tick. Kept
// deliberately linear: tick() reads top-to-bottom exactly like Appendix A.1.

#include "core/tick/tick_loop.h"

#include "core/ecs/world.h"
#include "core/hierarchy/hierarchy.h"
#include "core/journal/record.h"
#include "core/journal/writer.h"

#include <algorithm>
#include <string>
#include <utility>

namespace midday::tick {

namespace {

[[nodiscard]] std::size_t index_of(Phase phase) {
    return static_cast<std::size_t>(phase);
}

[[nodiscard]] base::Json phase_details(Phase phase) {
    base::Json details = base::Json::object();
    details.set("phase", to_string(phase));
    return details;
}

[[nodiscard]] base::Error reentrant_error() {
    return {
        "tick.reentrant", "tick() called while a tick is already running", base::Json::object()};
}

[[nodiscard]] base::Error hook_locked_error(std::string_view operation, Phase phase) {
    base::Json details = phase_details(phase);
    details.set("operation", operation);
    return {"tick.hook_locked",
            "phase hooks are boot wiring: add/remove is refused mid-tick",
            std::move(details)};
}

[[nodiscard]] base::Error phase_reserved_error(Phase phase) {
    return {"tick.phase_reserved",
            "hooks attach to open phases only (watchers..post)",
            phase_details(phase)};
}

[[nodiscard]] base::Error duplicate_hook_error(Phase phase) {
    return {"tick.duplicate_hook",
            "this hook is already registered on this phase",
            phase_details(phase)};
}

[[nodiscard]] base::Error hook_absent_error(Phase phase) {
    return {"tick.hook_absent", "no such hook registered on this phase", phase_details(phase)};
}

[[nodiscard]] base::Error null_key_error() {
    return {"tick.null_key", "inject_input refuses the null event key", base::Json::object()};
}

} // namespace

TickLoop::TickLoop(ecs::World& world,
                   hierarchy::Hierarchy& hierarchy,
                   bus::Bus& bus,
                   journal::Writer& journal,
                   TickLoopConfig config)
    : world_(&world), hierarchy_(&hierarchy), bus_(&bus), journal_(&journal), config_(config),
      dt_(1.0 / static_cast<double>(config.ticks_per_second == 0 ? 60 : config.ticks_per_second)) {}

// ---- phase hooks -----------------------------------------------------------

std::optional<base::Error> TickLoop::add_hook(Phase phase, PhaseHook& hook) {
    if (in_tick_)
        return hook_locked_error("add_hook", phase);
    if (!is_open_phase(phase))
        return phase_reserved_error(phase);
    std::vector<PhaseHook*>& list = hooks_[index_of(phase)];
    if (std::ranges::find(list, &hook) != list.end())
        return duplicate_hook_error(phase);
    list.push_back(&hook);
    return std::nullopt;
}

std::optional<base::Error> TickLoop::remove_hook(Phase phase, PhaseHook& hook) {
    if (in_tick_)
        return hook_locked_error("remove_hook", phase);
    if (!is_open_phase(phase))
        return phase_reserved_error(phase);
    std::vector<PhaseHook*>& list = hooks_[index_of(phase)];
    const auto found = std::ranges::find(list, &hook);
    if (found == list.end())
        return hook_absent_error(phase);
    list.erase(found); // registration order of the survivors is preserved
    return std::nullopt;
}

std::uint32_t TickLoop::hook_count(Phase phase) const {
    return static_cast<std::uint32_t>(hooks_[index_of(phase)].size());
}

// ---- input feed --------------------------------------------------------------

std::optional<base::Error>
TickLoop::inject_input(bus::EventKey key, base::Name event, base::Json payload) {
    if (key.is_null())
        return null_key_error();
    input_queue_.push_back(InjectedInput{key, event, std::move(payload)});
    ++stats_.inputs_injected;
    return std::nullopt;
}

// ---- the tick ----------------------------------------------------------------

std::uint64_t TickLoop::phase_marker(Phase phase, std::uint64_t cause_id) {
    // The one journal-inherent allocation per phase (header cost model).
    base::Json payload = base::Json::object();
    payload.set("phase", to_string(phase));
    return journal_->record(
        tick_, journal::Tier::Flight, "tick.phase", cause_id, std::move(payload));
}

std::optional<base::Error> TickLoop::tick() {
    if (in_tick_)
        return reentrant_error();
    in_tick_ = true;
    const auto finish = [this](std::optional<base::Error> error) {
        in_tick_ = false;
        return error;
    };
    const auto refused = [this](Phase phase) {
        base::Json details = phase_details(phase);
        if (journal_->status().has_value())
            details.set("journal", journal_->status()->code);
        return base::Error{"tick.journal_refused",
                           "the journal refused the phase marker: unjournaled ticks do not exist",
                           std::move(details)};
    };

    // Phase 1: tick-begin — the counter, the bus's tick stamp, the tick's
    // root journal record.
    ++tick_;
    bus_->set_tick(tick_);
    const std::uint64_t tick_record_id = phase_marker(Phase::kTickBegin, 0);
    if (tick_record_id == 0)
        return finish(refused(Phase::kTickBegin));

    // Phase 2: input — drain the injection queue onto the bus.
    if (phase_marker(Phase::kInput, tick_record_id) == 0)
        return finish(refused(Phase::kInput));
    if (auto error = run_input_phase())
        return finish(std::move(error));

    // Phases 3–7: watchers · sequences · update · physics · post — the open
    // phases, hooks in registration order.
    for (auto phase :
         {Phase::kWatchers, Phase::kSequences, Phase::kUpdate, Phase::kPhysics, Phase::kPost}) {
        const std::uint64_t phase_record_id = phase_marker(phase, tick_record_id);
        if (phase_record_id == 0)
            return finish(refused(phase));
        run_hooks(phase, phase_record_id, tick_record_id);
    }

    // Phase 8: structural apply — THE deterministic mutation point.
    const std::uint64_t structural_record_id =
        phase_marker(Phase::kStructuralApply, tick_record_id);
    if (structural_record_id == 0)
        return finish(refused(Phase::kStructuralApply));
    if (auto error = run_structural_apply(structural_record_id))
        return finish(std::move(error));

    // Phase 9: tick-end — frame-packet capture, journal flush cadence.
    if (phase_marker(Phase::kTickEnd, tick_record_id) == 0)
        return finish(refused(Phase::kTickEnd));
    if (auto error = run_tick_end())
        return finish(std::move(error));

    ++stats_.ticks;
    return finish(std::nullopt);
}

std::optional<base::Error> TickLoop::run_input_phase() {
    // The cutoff: inputs injected from here on (by listeners/hooks) land in
    // input_queue_ and deliver NEXT tick. Swapping reuses both capacities —
    // no steady-state allocation.
    input_drain_.clear();
    input_drain_.swap(input_queue_);
    for (InjectedInput& input : input_drain_) {
        // Root records (cause 0): inputs enter the sim from outside.
        bus::TriggerResult result = bus_->trigger(input.key, input.event, input.payload, 0);
        if (result.record_id != 0) {
            ++stats_.inputs_delivered;
            continue;
        }
        if (result.error.has_value() && result.error->code == "bus.journal_refused")
            return std::move(result.error); // the run is dead — stop the tick
        ++stats_.inputs_refused;            // bus journaled the refusal; the heartbeat goes on
    }
    return std::nullopt;
}

void TickLoop::run_hooks(Phase phase, std::uint64_t phase_record_id, std::uint64_t tick_record_id) {
    PhaseContext context;
    context.phase = phase;
    context.tick = tick_;
    context.dt = dt_;
    context.phase_record_id = phase_record_id;
    context.tick_record_id = tick_record_id;
    // Index iteration over a vector frozen by the mid-tick hook lock.
    const std::vector<PhaseHook*>& list = hooks_[index_of(phase)];
    for (PhaseHook* hook : list)
        hook->on_phase(*this, context);
}

std::optional<base::Error> TickLoop::run_structural_apply(std::uint64_t phase_record_id) {
    // Queued spawns/despawns/reparents apply in queue order; the hierarchy's
    // reparent handler and despawn observer run inside the flush, and tree
    // order indices rebuild lazily off the new topology.
    if (auto error = world_->flush_structural())
        return error;
    // The ONE structural-apply extension slot (m1-prefab-spawn): realizes
    // queued prefab spawns (adopt, machine instantiate — the enter chain,
    // A.1 phase 8) and fires despawn lifecycle events, now that the entities
    // it reserved are provably alive/dead. Runs strictly AFTER the flush
    // above returns (never reentrant into it) and BEFORE propagate, so a
    // realized prefab's local transform lands in this tick's settle.
    if (realizer_) {
        if (auto error = realizer_(phase_record_id))
            return error;
    }
    // World transforms recompute parents-before-children — after this point
    // the tick's spatial state is final and the frame packet may capture it.
    hierarchy_->propagate();
    return std::nullopt;
}

std::optional<base::Error> TickLoop::run_tick_end() {
    // The extraction seam: sim writes are closed; publish this tick's
    // end-state packet (reader keeps packet N while N+1 is being written).
    FramePacket& packet = packets_.begin_write();
    packet.tick = tick_;
    packet.dt_seconds = dt_;
    packet.alpha_hint = accumulator_ / dt_;
    packet.transform_snapshot = tick_; // opaque version token (frame_packet.h)
    packets_.publish();

    if (config_.journal_flush_stride != 0 && tick_ % config_.journal_flush_stride == 0) {
        if (auto error = journal_->flush())
            return error;
    }
    return std::nullopt;
}

// ---- batch stepping ----------------------------------------------------------

std::optional<base::Error> TickLoop::tick(std::uint64_t count) {
    for (std::uint64_t i = 0; i < count; ++i) {
        if (auto error = tick())
            return error;
    }
    return std::nullopt;
}

std::optional<base::Error> TickLoop::run_to_tick(std::uint64_t target) {
    while (tick_ < target) {
        if (auto error = tick())
            return error;
    }
    return std::nullopt;
}

AdvanceResult TickLoop::advance(double elapsed_seconds) {
    AdvanceResult result;
    if (elapsed_seconds > 0.0)
        accumulator_ += elapsed_seconds;
    // The spiral-of-death guard: never owe more than the catch-up budget;
    // the surplus is dropped LOUDLY (stats), never silently.
    const double budget = dt_ * static_cast<double>(config_.max_catchup_steps);
    if (accumulator_ > budget) {
        stats_.dropped_seconds += accumulator_ - budget;
        ++stats_.catchup_clamps;
        accumulator_ = budget;
    }
    while (accumulator_ >= dt_) {
        accumulator_ -= dt_; // consume BEFORE stepping: tick-end's alpha_hint
                             // sees the true leftover
        if (auto error = tick()) {
            result.error = std::move(error);
            return result;
        }
        ++result.ticks_run;
    }
    result.alpha = accumulator_ / dt_;
    return result;
}

} // namespace midday::tick
