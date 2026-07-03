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

# quickjs-ng 0.15.1 — the embedded JS engine for the TypeScript script tier
# (spec section 7). Core library only: quickjs-libc is deliberately not
# vendored, so scripts get no OS, filesystem, or timer surface — the module
# loader and host hooks in ts/runtime are the only doors. Interpreter-only
# (no JIT), which is exactly why QuickJS is the first runtime: identical
# behavior on iOS where JIT pages are forbidden. Determinism audit
# (m0-quickjs-ts-toolchain): wall-clock reaches JS only through Date,
# performance.now, and the Math.random seed — all three are poisoned at the
# JS level by the sim-profile prelude in ts/runtime/script_runtime.cpp, never
# by patching this source. Sim scripts execute inside this interpreter, so it
# compiles under the same deterministic-FP contract as first-party sim code.
# Upstream's build applies _GNU_SOURCE everywhere plus the two Win32 macros;
# it links libm and Threads (C11 atomics) where present.
enable_language(C)
add_library(quickjs STATIC
    quickjs/quickjs.c
    quickjs/libregexp.c
    quickjs/libunicode.c
    quickjs/dtoa.c
)
add_library(quickjs::quickjs ALIAS quickjs)
set_target_properties(quickjs PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_include_directories(quickjs SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/quickjs)
target_compile_definitions(quickjs PRIVATE _GNU_SOURCE)
if(WIN32)
    target_compile_definitions(quickjs PRIVATE WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601)
endif()
find_package(Threads QUIET)
if(TARGET Threads::Threads)
    target_link_libraries(quickjs PUBLIC Threads::Threads)
endif()
if(UNIX)
    target_link_libraries(quickjs PUBLIC m ${CMAKE_DL_LIBS})
endif()
if(NOT MSVC)
    # Always optimize the interpreter, even in Debug presets: its recursion
    # guard measures REAL C-stack bytes, and -O0 frames are ~4x larger — the
    # vendored TypeScript compiler's parser would exhaust the JS stack budget
    # in dev builds. We never debug vendored quickjs at -O0.
    # (MSVC keeps per-config flags: /O2 conflicts with Debug's /RTC1; the
    # midday binary reserves 8 MiB of stack there instead — cli/CMakeLists.)
    target_compile_options(quickjs PRIVATE -O2)
endif()
midday_apply_deterministic_fp(quickjs)

# zstd 1.5.7 — journal stream compression (single-file amalgamation from the
# upstream build/single_file_libs generator, regenerated WITHOUT
# ZSTD_MULTITHREAD: single-threaded compression is a determinism requirement
# for journal bytes and drops the pthread/Win32-thread dependency entirely
# (D-BUILD-031). C source; compiles clean on gcc/clang/AppleClang/MSVC.
enable_language(C)
add_library(zstd STATIC ${CMAKE_CURRENT_SOURCE_DIR}/zstd/zstd.c)
add_library(zstd::zstd ALIAS zstd)
target_include_directories(zstd SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/zstd)
