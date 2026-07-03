# ts/runtime — the embedded QuickJS script runtime (m0-quickjs-ts-toolchain)
# plus the batch-first binding layer (m0-batch-bindings). Sim scripts execute
# in here: deterministic-FP applies to the embedding and to the vendored
# interpreter itself (third_party/CMakeLists.txt).

add_library(midday_script_runtime STATIC
    script_runtime.cpp
    batch_views.cpp
)
add_library(midday::script_runtime ALIAS midday_script_runtime)

target_link_libraries(midday_script_runtime
    PUBLIC midday::options midday::base midday::ecs midday::math midday::reflect
    PRIVATE quickjs::quickjs)
midday_apply_deterministic_fp(midday_script_runtime)

# The benchmark harness (midday script bench) lives in its OWN target: it
# drives fixtures through the TS toolchain, and the toolchain already links
# the runtime — same-target placement would cycle the dependency graph.
add_library(midday_batch_bench STATIC
    batch_bench.cpp
)
add_library(midday::batch_bench ALIAS midday_batch_bench)

# midday_ts_toolchain by real name: ts/toolchain is added AFTER ts/runtime
# and alias targets cannot be forward-referenced (plain target names can).
target_link_libraries(midday_batch_bench
    PUBLIC midday::options midday::base midday::script_runtime
    PRIVATE midday_ts_toolchain midday::ecs midday::math midday::reflect xxhash::xxhash)
midday_apply_deterministic_fp(midday_batch_bench)

# Tests as an OBJECT library so doctest self-registration survives linking
# into the midday binary (D-BUILD-014 pattern).
add_library(midday_script_runtime_tests OBJECT
    script_runtime_test.cpp
    batch_views_test.cpp
    batch_bench_test.cpp
)
target_link_libraries(midday_script_runtime_tests
    PRIVATE midday::script_runtime midday::batch_bench doctest::doctest)
midday_apply_deterministic_fp(midday_script_runtime_tests)
