# codegen_bootstrap — TEMPORARY native bootstrap generator (m0-codegen-bootstrap).
# Retired to bootstrap duty once m0-codegen-selfhost byte-matches it (README).
# Library/tool split: all logic lives in the library so `midday selftest
# --filter codegen.*` covers it; main.cpp is argv + file I/O only.

add_library(midday_codegen_bootstrap STATIC
    codegen.cpp
    dts_emit.cpp
    manifest_emit.cpp
    docs_emit.cpp
    bindings_emit.cpp
)
add_library(midday::codegen_bootstrap ALIAS midday_codegen_bootstrap)

target_link_libraries(midday_codegen_bootstrap
    PUBLIC midday::options midday::base
    PRIVATE midday::api midday::reflect)

# Generated artifacts are golden byte-compare material: sim-grade FP discipline.
midday_apply_deterministic_fp(midday_codegen_bootstrap)

add_executable(codegen_bootstrap main.cpp)
target_link_libraries(codegen_bootstrap PRIVATE
    midday::options midday::codegen_bootstrap midday::base)
midday_apply_deterministic_fp(codegen_bootstrap)

# The exit-test contract addresses the tool as build/<preset>/tools/codegen_bootstrap.
set_target_properties(codegen_bootstrap PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tools)

# Tests as an OBJECT library so doctest self-registration survives linking
# into the midday binary (D-BUILD-014 pattern). The live-document tests
# reference cli/ symbols, resolved at the midday executable link.
add_library(midday_codegen_bootstrap_tests OBJECT
    codegen_test.cpp
)
target_link_libraries(midday_codegen_bootstrap_tests
    PRIVATE midday::codegen_bootstrap midday::api doctest::doctest)
midday_apply_deterministic_fp(midday_codegen_bootstrap_tests)
