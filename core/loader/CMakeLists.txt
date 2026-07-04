# core/loader — the PERMANENT strict-YAML loader subset (m0-yaml-loader-run):
# scene/machine/events text -> the existing desc aggregates -> World/Hierarchy
# instantiation. m1-scene-format extends this library IN PLACE (one loader,
# ever). Vendored rapidyaml is quarantined to yaml_parse.cpp.

add_library(midday_loader STATIC
    yaml_parse.cpp
    parse_util.cpp
    events_load.cpp
    machine_load.cpp
    machine_parts.cpp
    scene_load.cpp
    spawn.cpp
)
add_library(midday::loader ALIAS midday_loader)
target_link_libraries(midday_loader
    PUBLIC midday::options midday::base midday::math midday::reflect midday::expr
           midday::ecs midday::hierarchy midday::bus midday::statechart midday::physics
    PRIVATE ryml::ryml)
midday_apply_deterministic_fp(midday_loader)

# Tests as an OBJECT library so doctest self-registration survives linking
# into the midday binary (D-BUILD-014 pattern).
add_library(midday_loader_tests OBJECT
    yaml_test.cpp
    events_load_test.cpp
    machine_load_test.cpp
    scene_load_test.cpp
    spawn_test.cpp
)
target_link_libraries(midday_loader_tests
    PRIVATE midday::loader midday::journal midday::tick doctest::doctest)
midday_apply_deterministic_fp(midday_loader_tests)
