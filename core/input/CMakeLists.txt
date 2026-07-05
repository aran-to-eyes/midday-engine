# core/input — named action maps, the runtime action-state cache + the
# Godot-precedent virtual-stick get_vector query (m1-input-actions), and the
# public/stable/permanent synthetic injection API the testkit is built on.
# The *.input.yaml / *.input_profile.yaml loaders live in core/loader (one
# loader, ever) — this library is the RUNTIME half.

add_library(midday_input STATIC
    action_state.cpp
    inject.cpp
)
add_library(midday::input ALIAS midday_input)
target_link_libraries(midday_input
    PUBLIC midday::options midday::base midday::math midday::bus midday::tick midday::loader)
midday_apply_deterministic_fp(midday_input)

# Tests as an OBJECT library so doctest self-registration survives linking
# into the midday binary (D-BUILD-014 pattern).
add_library(midday_input_tests OBJECT
    action_state_test.cpp
    inject_test.cpp
)
target_link_libraries(midday_input_tests
    PRIVATE midday::input midday::journal doctest::doctest)
midday_apply_deterministic_fp(midday_input_tests)
