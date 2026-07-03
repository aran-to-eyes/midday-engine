# ts/codegen — the SELF-HOSTED code generator (m0-codegen-selfhost). The
# generator itself is TypeScript DATA (generator.ts + modules, built at
# runtime through the toolchain's content-hash cache); this target is only
# the C++ seam that runs it on the embedded QuickJS. Authoritative for the
# committed api/ artifacts; tools/codegen_bootstrap stays equivalence-pinned.

add_library(midday_codegen_selfhost STATIC
    selfhost.cpp
)
add_library(midday::codegen_selfhost ALIAS midday_codegen_selfhost)

target_link_libraries(midday_codegen_selfhost
    PUBLIC midday::options midday::base
    PRIVATE midday::ts_toolchain midday::script_runtime)
midday_apply_deterministic_fp(midday_codegen_selfhost)

# Equivalence harness as an OBJECT library in the midday binary (D-BUILD-014
# pattern); it drives BOTH generators, so it links the bootstrap library too.
add_library(midday_codegen_selfhost_tests OBJECT
    selfhost_test.cpp
)
target_link_libraries(midday_codegen_selfhost_tests
    PRIVATE midday::codegen_selfhost midday::codegen_bootstrap midday::api doctest::doctest)
midday_apply_deterministic_fp(midday_codegen_selfhost_tests)
