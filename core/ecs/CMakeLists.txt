# core/ecs — sparse-set ECS (m0-ecs-core): paged sparse arrays, packed pools
# with first-class active bits, generational EntityRef handles, non-owning
# views, deferred structural queue, reflect registration bridge. Owning
# packed groups are design-only here (group.h) and land at m2-jobs.

add_library(midday_ecs STATIC
    world.cpp
)
add_library(midday::ecs ALIAS midday_ecs)

target_link_libraries(midday_ecs
    PUBLIC midday::options midday::base midday::reflect)

# The ECS is THE sim data structure: the deterministic-FP contract applies.
midday_apply_deterministic_fp(midday_ecs)

# Tests as an OBJECT library so doctest self-registration survives linking
# into the midday binary (same pattern as midday_base_tests, D-BUILD-014).
add_library(midday_ecs_tests OBJECT
    determinism_test.cpp
    entity_test.cpp
    pool_test.cpp
    sparse_set_test.cpp
    view_test.cpp
    world_test.cpp
)
target_link_libraries(midday_ecs_tests
    PRIVATE midday::ecs midday::math doctest::doctest xxhash::xxhash)
midday_apply_deterministic_fp(midday_ecs_tests)
