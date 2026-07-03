# core/physics — the minimal deterministic physics server over vendored
# JoltPhysics (m0-jolt-minimal). Jolt is a PRIVATE implementation detail:
# no Jolt header escapes physics_server.cpp, so nothing above this library
# ever sees a Jolt type (the server-interface seam, spec section 6).

add_library(midday_physics STATIC
    physics_server.cpp
)
add_library(midday::physics ALIAS midday_physics)

target_link_libraries(midday_physics
    PUBLIC midday::options midday::base midday::math midday::ecs midday::hierarchy
           midday::bus midday::tick
    PRIVATE jolt::jolt)

# The physics step IS sim state: the deterministic-FP contract applies.
midday_apply_deterministic_fp(midday_physics)

# Tests as an OBJECT library so doctest self-registration survives linking
# into the midday binary (D-BUILD-014 pattern).
add_library(midday_physics_tests OBJECT
    physics_contacts_test.cpp
    physics_determinism_test.cpp
    physics_server_test.cpp
)
target_link_libraries(midday_physics_tests
    PRIVATE midday::physics midday::tick midday::journal midday::reflect doctest::doctest)
midday_apply_deterministic_fp(midday_physics_tests)
