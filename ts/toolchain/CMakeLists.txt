# ts/toolchain — the TypeScript compiler-as-data toolchain: typecheck + lint
# + transpile on the embedded QuickJS, with the XXH3 content-hash cache.
# driver.js and the vendored typescript.js are DATA read at runtime, never
# compiled or linked.

add_library(midday_ts_toolchain STATIC
    toolchain.cpp
)
add_library(midday::ts_toolchain ALIAS midday_ts_toolchain)

target_link_libraries(midday_ts_toolchain
    PUBLIC midday::options midday::base midday::script_runtime
    PRIVATE xxhash::xxhash)
midday_apply_deterministic_fp(midday_ts_toolchain)

add_library(midday_ts_toolchain_tests OBJECT
    toolchain_test.cpp
)
target_link_libraries(midday_ts_toolchain_tests
    PRIVATE midday::ts_toolchain doctest::doctest)
midday_apply_deterministic_fp(midday_ts_toolchain_tests)
