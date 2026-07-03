# core/statechart — the statechart entity runtime (m0-statechart-core):
# Appendix A.2 transition algorithm inline on bus events, A.2.1 hook order,
# states as hierarchy nodes, `when:` watchers in phase 3, voiding + region
# marking, everything journaled. Proven C++-first — no QuickJS anywhere here.
# Plus dope-sheet sequences (m0-sequences): tick-locked trigger/span tracks
# in phase 4, interruption cleanup inside the exit chain, loop/hold/history.

add_library(midday_statechart STATIC
    statechart.cpp
    instantiate.cpp
    transitions.cpp
    watchers.cpp
    sequences.cpp
    hooks.cpp
)
add_library(midday::statechart ALIAS midday_statechart)

target_link_libraries(midday_statechart
    PUBLIC midday::options midday::base midday::ecs midday::hierarchy midday::journal
           midday::bus midday::tick midday::expr midday::reflect)

# Transition filters/watchers evaluate sim expressions: the FP contract applies.
midday_apply_deterministic_fp(midday_statechart)

# Tests as an OBJECT library so doctest self-registration survives linking
# into the midday binary (D-BUILD-014 pattern).
add_library(midday_statechart_tests OBJECT
    hook_order_test.cpp
    transition_test.cpp
    cascade_test.cpp
    watcher_test.cpp
    sequence_test.cpp
    sequence_interrupt_test.cpp
    determinism_test.cpp
)
target_link_libraries(midday_statechart_tests PRIVATE midday::statechart doctest::doctest)
midday_apply_deterministic_fp(midday_statechart_tests)
