# core/base — primitives every other subsystem stands on (m0-core-primitives):
# JSON IO, structured Error envelope, interned Names, frame arena, JSONL logging.

add_library(midday_base STATIC
    arena.cpp
    error.cpp
    json_parse.cpp
    json_write.cpp
    log.cpp
    name.cpp
)
add_library(midday::base ALIAS midday_base)

target_link_libraries(midday_base
    PUBLIC midday::options
    PRIVATE xxhash::xxhash)

# Core code is sim code: the deterministic-FP contract applies.
midday_apply_deterministic_fp(midday_base)

# Tests ship as an OBJECT library so doctest's self-registration survives
# linking into the midday binary (static libs would drop unreferenced TUs).
add_library(midday_base_tests OBJECT
    arena_test.cpp
    error_test.cpp
    json_test.cpp
    log_test.cpp
    name_test.cpp
)
target_link_libraries(midday_base_tests PRIVATE midday::base doctest::doctest)
midday_apply_deterministic_fp(midday_base_tests)
