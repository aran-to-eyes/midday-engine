# core/hierarchy — runtime entity tree (m0-scene-hierarchy): ECS-resident
# topology, deterministic depth-first tree order, dirty-flagged transform
# propagation, subtree activation (the states-as-nodes substrate).

add_library(midday_hierarchy STATIC
    activation.cpp
    topology.cpp
    transforms.cpp
    tree_order.cpp
)
add_library(midday::hierarchy ALIAS midday_hierarchy)

target_link_libraries(midday_hierarchy
    PUBLIC midday::options midday::base midday::math midday::reflect midday::ecs)

# Transform propagation is sim math: the deterministic-FP contract applies.
midday_apply_deterministic_fp(midday_hierarchy)

# Tests as an OBJECT library so doctest self-registration survives linking
# into the midday binary (same pattern as midday_base_tests, D-BUILD-014).
add_library(midday_hierarchy_tests OBJECT
    activation_test.cpp
    topology_test.cpp
    transform_test.cpp
    tree_order_test.cpp
)
target_link_libraries(midday_hierarchy_tests
    PRIVATE midday::hierarchy doctest::doctest xxhash::xxhash)
midday_apply_deterministic_fp(midday_hierarchy_tests)
