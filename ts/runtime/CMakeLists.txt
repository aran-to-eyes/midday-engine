# ts/runtime — the embedded QuickJS script runtime (m0-quickjs-ts-toolchain).
# Sim scripts execute in here: deterministic-FP applies to the embedding and
# to the vendored interpreter itself (third_party/CMakeLists.txt).

add_library(midday_script_runtime STATIC
    script_runtime.cpp
)
add_library(midday::script_runtime ALIAS midday_script_runtime)

target_link_libraries(midday_script_runtime
    PUBLIC midday::options midday::base
    PRIVATE quickjs::quickjs)
midday_apply_deterministic_fp(midday_script_runtime)

# Tests as an OBJECT library so doctest self-registration survives linking
# into the midday binary (D-BUILD-014 pattern).
add_library(midday_script_runtime_tests OBJECT
    script_runtime_test.cpp
)
target_link_libraries(midday_script_runtime_tests
    PRIVATE midday::script_runtime doctest::doctest)
midday_apply_deterministic_fp(midday_script_runtime_tests)
