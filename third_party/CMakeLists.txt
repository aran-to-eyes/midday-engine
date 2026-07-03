# Vendored third-party targets. Every directory here is pinned in
# LICENSES/manifest.json (scripts/license_scan.py enforces this in CI).

# doctest 2.4.12 — in-binary test framework for `midday selftest`.
add_library(doctest INTERFACE)
add_library(doctest::doctest ALIAS doctest)
target_include_directories(doctest SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/doctest)

# xxhash 0.8.3 — content hashing (interned Name ids now; journal/golden
# hashes later). Header-only via XXH_INLINE_ALL.
add_library(xxhash INTERFACE)
add_library(xxhash::xxhash ALIAS xxhash)
target_include_directories(xxhash SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/xxhash)
