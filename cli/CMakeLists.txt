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
    verbs/script.cpp
    verbs/run.cpp
    verbs/journal.cpp
    verbs/run_test.cpp
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
    $<TARGET_OBJECTS:midday_statechart_tests>
    $<TARGET_OBJECTS:midday_loader_tests>
    $<TARGET_OBJECTS:midday_physics_tests>
    $<TARGET_OBJECTS:midday_api_tests>
    $<TARGET_OBJECTS:midday_codegen_bootstrap_tests>
    $<TARGET_OBJECTS:midday_codegen_selfhost_tests>
    $<TARGET_OBJECTS:midday_script_runtime_tests>
    $<TARGET_OBJECTS:midday_ts_toolchain_tests>
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
    midday::statechart
    midday::loader
    midday::physics
    midday::api
    midday::codegen_bootstrap
    midday::codegen_selfhost
    midday::script_runtime
    midday::batch_bench
    midday::ts_toolchain
    doctest::doctest)
target_compile_definitions(midday PRIVATE MIDDAY_VERSION="${PROJECT_VERSION}")

# The CLI hosts the deterministic sim (midday run) from m0-yaml-loader-run on;
# the FP contract applies from commit 1.
midday_apply_deterministic_fp(midday)

# Windows reserves 1 MiB of thread stack by default; the TS toolchain gives
# QuickJS a 6 MiB budget for the vendored compiler (ts/toolchain), so the
# binary must actually have that stack. POSIX main threads default to 8 MiB.
if(MSVC)
    target_link_options(midday PRIVATE /STACK:8388608)
endif()

# The exit-test contract addresses the binary as build/<preset>/midday.
set_target_properties(midday PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
