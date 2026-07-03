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

# fast_float 8.2.9 — correctly-rounded, locale-free double parsing. The JSON
# parser uses it instead of std::from_chars(double), which old libc++ lacks
# (D-BUILD-015). Header-only.
add_library(fast_float INTERFACE)
add_library(fast_float::fast_float ALIAS fast_float)
target_include_directories(fast_float SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/fast_float)

# dragonbox 1.1.3 — shortest round-trip decimal digits for double emission;
# with the writer's formatting rule this makes JSON double bytes identical on
# every platform/toolchain (D-BUILD-015). Header-only.
add_library(dragonbox INTERFACE)
add_library(dragonbox::dragonbox ALIAS dragonbox)
target_include_directories(dragonbox SYSTEM INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/dragonbox)

# zstd 1.5.7 — journal stream compression (single-file amalgamation from the
# upstream build/single_file_libs generator, regenerated WITHOUT
# ZSTD_MULTITHREAD: single-threaded compression is a determinism requirement
# for journal bytes and drops the pthread/Win32-thread dependency entirely
# (D-BUILD-031). C source; compiles clean on gcc/clang/AppleClang/MSVC.
enable_language(C)
add_library(zstd STATIC ${CMAKE_CURRENT_SOURCE_DIR}/zstd/zstd.c)
add_library(zstd::zstd ALIAS zstd)
target_include_directories(zstd SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/zstd)
