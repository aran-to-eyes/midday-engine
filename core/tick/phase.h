// core/tick/phase.h — the nine phases of the fixed tick, Appendix A.1
// (NORMATIVE): their order, their journal spellings, and which of them
// accept phase hooks. This enum IS the phase order — TickLoop iterates it
// numerically, and the tick.phases exit test asserts the journal shows
// exactly this cycle every tick.

#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace midday::tick {

// Appendix A.1, phases 1..9, in contractual order. The numeric values are
// the execution order; reordering this enum is a spec violation.
enum class Phase : std::uint8_t {
    kTickBegin = 0,       // 1: tick counter++, journal tick marker
    kInput = 1,           // 2: injected input queue drains onto the bus
    kWatchers = 2,        // 3: `when:` condition watchers (statechart attaches)
    kSequences = 3,       // 4: dope-sheet playheads advance (m0-sequences attaches)
    kUpdate = 4,          // 5: pre-update systems + onFixedUpdate hooks
    kPhysics = 5,         // 6: fixed-dt physics step (m0-jolt-minimal attaches)
    kPost = 6,            // 7: post-update systems
    kStructuralApply = 7, // 8: THE deterministic mutation point (engine-owned)
    kTickEnd = 8,         // 9: journal flush, frame-packet capture (engine-owned)
};

inline constexpr std::uint32_t kPhaseCount = 9;

// The journal spelling of each phase — the `phase` field of every
// {kind:"tick.phase"} marker. Stable dotted/hyphenated identifiers, pinned
// by the tick.phases test; changing a spelling is a journal-format change.
inline constexpr std::array<std::string_view, kPhaseCount> kPhaseNames = {
    "tick-begin",
    "input",
    "watchers",
    "sequences",
    "update",
    "physics",
    "post",
    "structural-apply",
    "tick-end",
};

[[nodiscard]] constexpr std::string_view to_string(Phase phase) {
    return kPhaseNames[static_cast<std::uint32_t>(phase)];
}

// Hooks attach only to the five OPEN phases (watchers, sequences, update,
// physics, post). tick-begin, input, structural-apply, and tick-end are
// engine-owned: their bodies are the loop's own contract (input feeds
// through inject_input, structural mutation through the ECS queue, render
// extraction through the frame-packet seam) — never through a hook.
[[nodiscard]] constexpr bool is_open_phase(Phase phase) {
    return phase >= Phase::kWatchers && phase <= Phase::kPost;
}

} // namespace midday::tick
