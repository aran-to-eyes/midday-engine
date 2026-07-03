# midday — the engine binary. Verbs live one-per-file under verbs/; the
# doctest registry is compiled in (spec section 14) and run via `midday selftest`.
# Core tests link in as OBJECT libraries so their self-registration survives.

add_executable(midday
    main.cpp
    envelope.cpp
    parse.cpp
    help.cpp
    verbs/registry.cpp
    verbs/version.cpp
    verbs/selftest.cpp
    verbs/help.cpp
    verbs/api.cpp
    verbs/envelope_test.cpp
    verbs/framework_test.cpp
    $<TARGET_OBJECTS:midday_base_tests>
    $<TARGET_OBJECTS:midday_math_tests>
    $<TARGET_OBJECTS:midday_reflect_tests>
    $<TARGET_OBJECTS:midday_expr_tests>
    $<TARGET_OBJECTS:midday_ecs_tests>
    $<TARGET_OBJECTS:midday_hierarchy_tests>
    $<TARGET_OBJECTS:midday_journal_tests>
    $<TARGET_OBJECTS:midday_bus_tests>
    $<TARGET_OBJECTS:midday_tick_tests>
    $<TARGET_OBJECTS:midday_api_tests>
    $<TARGET_OBJECTS:midday_codegen_bootstrap_tests>
)

target_link_libraries(midday PRIVATE
    midday::options
    midday::base
    midday::math
    midday::reflect
    midday::expr
    midday::ecs
    midday::hierarchy
    midday::journal
    midday::bus
    midday::tick
    midday::api
    midday::codegen_bootstrap
    doctest::doctest)
target_compile_definitions(midday PRIVATE MIDDAY_VERSION="${PROJECT_VERSION}")

# The CLI hosts the deterministic sim (midday run) from m0-yaml-loader-run on;
# the FP contract applies from commit 1.
midday_apply_deterministic_fp(midday)

# The exit-test contract addresses the binary as build/<preset>/midday.
set_target_properties(midday PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
