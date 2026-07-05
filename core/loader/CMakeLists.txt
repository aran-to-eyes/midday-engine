# core/loader — the PERMANENT strict-YAML loader subset (m0-yaml-loader-run):
# scene/machine/events text -> the existing desc aggregates -> World/Hierarchy
# instantiation. m1-scene-format extends this library IN PLACE (one loader,
# ever). Vendored rapidyaml is quarantined to yaml_parse.cpp.

add_library(midday_loader STATIC
    yaml_parse.cpp
    yaml_emit.cpp
    parse_util.cpp
    format_schema.cpp
    format_schema_validate.cpp
    events_load.cpp
    input_load.cpp
    machine_load.cpp
    machine_parts.cpp
    machine_components.cpp
    scene_load.cpp
    scene_components.cpp
    scene_prefab.cpp
    entity_load.cpp
    override.cpp
    component_vocab.cpp
    generic_components.cpp
    asset_ref_parse.cpp
    machine_emit.cpp
    spawn.cpp
    prefab_instantiate.cpp
    prefab_spawn.cpp
    uid.cpp
    uid_registry.cpp
    asset_ref.cpp
    project_schema.cpp
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
    yaml_emit_test.cpp
    format_schema_test.cpp
    events_load_test.cpp
    input_load_test.cpp
    machine_load_test.cpp
    scene_load_test.cpp
    entity_load_test.cpp
    override_test.cpp
    component_vocab_test.cpp
    generic_components_test.cpp
    machine_emit_test.cpp
    spawn_test.cpp
    prefab_spawn_test.cpp
    uid_test.cpp
    uid_registry_test.cpp
    asset_ref_test.cpp
    project_schema_test.cpp
)
target_link_libraries(midday_loader_tests
    PRIVATE midday::loader midday::journal midday::tick doctest::doctest)
midday_apply_deterministic_fp(midday_loader_tests)
