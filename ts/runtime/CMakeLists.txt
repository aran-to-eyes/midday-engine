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

# The TS state-script binding (m0-appendix-a-determinism): authored script:
# modules seated on statechart states through the generated hook seam. Own
# target for the same reason as the bench harness: it drives modules through
# the toolchain, which already links the runtime.
add_library(midday_state_script STATIC
    state_script.cpp
)
add_library(midday::state_script ALIAS midday_state_script)
target_link_libraries(midday_state_script
    PUBLIC midday::options midday::base midday::script_runtime
           midday::statechart midday::bus midday::ecs
    PRIVATE midday_ts_toolchain)
midday_apply_deterministic_fp(midday_state_script)

# The TS component-authoring binding (m1-ts-components): entity.get/tryGet/
# has/root's native primitives + this.emit's own-key trigger. Needs
# core::hierarchy too (entity.root() = Hierarchy::owner_of), which the base
# runtime does not link — own target, same rationale as state_script above.
add_library(midday_component_host STATIC
    component_host.cpp
)
add_library(midday::component_host ALIAS midday_component_host)
target_link_libraries(midday_component_host
    PUBLIC midday::options midday::base midday::script_runtime
           midday::ecs midday::bus midday::hierarchy)
midday_apply_deterministic_fp(midday_component_host)

# The TS world-spawn binding (m1-prefab-spawn): world.spawn(prefab)/
# despawn's native primitive — the ONE script-facing seam that queues a
# prefab spawn/despawn through the ECS structural queue (core/loader/
# prefab_spawn.h). Needs midday::loader (prefab resolution + override
# application) — own target, same rationale as component_host/state_script.
add_library(midday_world_host STATIC
    world_host.cpp
)
add_library(midday::world_host ALIAS midday_world_host)
target_link_libraries(midday_world_host
    PUBLIC midday::options midday::base midday::script_runtime
           midday::ecs midday::loader)
midday_apply_deterministic_fp(midday_world_host)

# Tests as an OBJECT library so doctest self-registration survives linking
# into the midday binary (D-BUILD-014 pattern).
add_library(midday_script_runtime_tests OBJECT
    script_runtime_test.cpp
    batch_views_test.cpp
    batch_bench_test.cpp
    state_script_test.cpp
    component_host_test.cpp
    world_host_test.cpp
)
target_link_libraries(midday_script_runtime_tests
    PRIVATE midday::script_runtime midday::batch_bench midday::state_script
            midday::component_host midday::world_host midday::statechart
            midday_ts_toolchain midday::journal midday::tick midday::hierarchy
            doctest::doctest)
midday_apply_deterministic_fp(midday_script_runtime_tests)
