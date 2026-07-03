# core/tick — the fixed tick loop (m0-tick-loop): Appendix A.1 phase order,
# ordered phase hooks, the headless input injection seam, deterministic
# stepping with max-catch-up clamping, and the double-buffered frame-packet
# extraction seam.

add_library(midday_tick STATIC
    tick_loop.cpp
)
add_library(midday::tick ALIAS midday_tick)

target_link_libraries(midday_tick
    PUBLIC midday::options midday::base midday::ecs midday::hierarchy midday::journal
           midday::bus midday::reflect)

# The loop IS the sim's clock: the deterministic-FP contract applies.
midday_apply_deterministic_fp(midday_tick)

# Tests as an OBJECT library so doctest self-registration survives linking
# into the midday binary (D-BUILD-014 pattern).
add_library(midday_tick_tests OBJECT
    phase_order_test.cpp
    stepping_test.cpp
    tick_loop_test.cpp
)
target_link_libraries(midday_tick_tests PRIVATE midday::tick doctest::doctest)
midday_apply_deterministic_fp(midday_tick_tests)
