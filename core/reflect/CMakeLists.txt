# core/reflect — ClassDB-equivalent reflection registry (m0-reflection-events):
# data-driven type model, class/event/function registration with XXH3 compat
# hashes, CORE->SERVERS->SCENE->TOOLS init levels, built-in event vocabulary.

add_library(midday_reflect STATIC
    builtin_events.cpp
    init_levels.cpp
    registry.cpp
    type_model.cpp
)
add_library(midday::reflect ALIAS midday_reflect)

target_link_libraries(midday_reflect
    PUBLIC midday::options midday::base
    PRIVATE xxhash::xxhash)

# Registry output feeds engine_api.json and golden byte-compares: sim-grade
# determinism discipline applies.
midday_apply_deterministic_fp(midday_reflect)

# Tests as an OBJECT library so doctest self-registration survives linking
# into the midday binary (same pattern as midday_base_tests, D-BUILD-014).
add_library(midday_reflect_tests OBJECT
    builtin_events_test.cpp
    init_levels_test.cpp
    registry_test.cpp
    type_model_test.cpp
)
target_link_libraries(midday_reflect_tests PRIVATE midday::reflect doctest::doctest)
midday_apply_deterministic_fp(midday_reflect_tests)
