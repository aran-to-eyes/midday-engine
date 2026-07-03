# core/math — the deterministic math stdlib (m0-math-stdlib): vectors,
# matrices, quaternions, transforms, splines, easing, intersections, seeded
# counter-based RNG streams, noise. Policy doc: core/math/README.md.

add_library(midday_math STATIC
    ease.cpp
    fixture.cpp
    noise.cpp
    rng.cpp
    spline.cpp
    xform.cpp
)
add_library(midday::math ALIAS midday_math)

target_link_libraries(midday_math
    PUBLIC midday::options midday::base
    PRIVATE xxhash::xxhash)

# Math IS sim code: the deterministic-FP contract applies (fast-math banned,
# FMA contraction pinned off — cmake/DeterministicFP.cmake).
midday_apply_deterministic_fp(midday_math)

# Tests as an OBJECT library so doctest self-registration survives linking
# into the midday binary (same pattern as midday_base_tests, D-BUILD-014).
add_library(midday_math_tests OBJECT
    determinism_test.cpp
    ease_test.cpp
    intersect_test.cpp
    mat_test.cpp
    noise_test.cpp
    quat_test.cpp
    rng_test.cpp
    spline_test.cpp
    vec_test.cpp
    xform_test.cpp
)
target_link_libraries(midday_math_tests PRIVATE midday::math doctest::doctest)
midday_apply_deterministic_fp(midday_math_tests)
