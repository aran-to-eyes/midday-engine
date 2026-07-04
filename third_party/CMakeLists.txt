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

# JoltPhysics 5.5.0 — THE physics engine (spec section 6), minimal
# deterministic server in M0 (m0-jolt-minimal). Vendored subset: the Jolt/
# library tree only (no Samples/UnitTests/TestFramework/Assets/Docs), kept
# byte-pristine like quickjs (D-BUILD-060 ethos). Compiled sources = every
# Jolt/ TU except ObjectStream/ — upstream's ENABLE_OBJECT_STREAM=OFF shape
# (Jolt's own scene serialization; our formats own that surface). The
# Renderer/ TUs compile to nothing without JPH_DEBUG_RENDERER, exactly as
# upstream builds them.
#
# Determinism config (D-BUILD-073, the interplay with DeterministicFP.cmake):
#   * JPH_CROSS_PLATFORM_DETERMINISTIC (PUBLIC — it changes inline header
#     code paths, so library and consumers must agree): Jolt substitutes its
#     own deterministic transcendental implementations and disables every
#     result-changing fast path. Upstream pairs this option with precise FP
#     and contraction off (Build/CMakeLists.txt) — here that half is exactly
#     midday_apply_deterministic_fp (/fp:precise; -fno-fast-math
#     -ffp-contract=off), the same contract every sim target carries.
#   * No -m arch flags: x86-64 baseline SSE2, arm64 NEON — Jolt's FMA paths
#     stay off (upstream: USE_FMADD is forced off when deterministic).
#   * No JPH_ENABLE_ASSERTS / JPH_PROFILE_ENABLED / JPH_DEBUG_RENDERER /
#     JPH_FLOATING_POINT_EXCEPTIONS_ENABLED in ANY config: dev and release
#     run the identical simulation code paths (no debug/release divergence,
#     the D-BUILD-025 ethos).
#   * Threading is a RUNTIME knob (core/physics config lock, D-BUILD-074):
#     M0 always steps through JobSystemSingleThreaded.
# file(GLOB) over a pinned, immutable vendor drop is deterministic by
# construction; CONFIGURE_DEPENDS makes a re-vendor self-healing.
file(GLOB_RECURSE JOLT_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/jolt/Jolt/*.cpp)
# Upstream's ENABLE_OBJECT_STREAM=OFF set: the ObjectStream* serializers and
# their TypeDeclarations TU are conditional; SerializableObject.cpp is NOT
# (the RTTI base every settings class derives from lives there).
list(FILTER JOLT_SOURCES EXCLUDE REGEX "/ObjectStream/(ObjectStream|TypeDeclarations)")
add_library(jolt STATIC ${JOLT_SOURCES})
add_library(jolt::jolt ALIAS jolt)
target_include_directories(jolt SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/jolt)
target_compile_definitions(jolt PUBLIC JPH_CROSS_PLATFORM_DETERMINISTIC)
if(NOT MSVC)
    # Always optimize the physics engine, even in Debug presets (the quickjs
    # precedent): a Debug Jolt makes the physics.min dual 1000-tick pile
    # fixtures dominate selftest, and identical flags across configs means
    # dev and release produce identical simulation bits. Same-build
    # determinism never depended on -O level (IEEE semantics are pinned by
    # the FP contract above); this just makes every preset the SAME build of
    # Jolt. (MSVC keeps per-config flags: /O2 conflicts with Debug /RTC1.)
    target_compile_options(jolt PRIVATE -O2)
endif()
find_package(Threads QUIET)
if(TARGET Threads::Threads)
    target_link_libraries(jolt PUBLIC Threads::Threads)
endif()
midday_apply_deterministic_fp(jolt)

# zstd 1.5.7 — journal stream compression (single-file amalgamation from the
# upstream build/single_file_libs generator, regenerated WITHOUT
# ZSTD_MULTITHREAD: single-threaded compression is a determinism requirement
# for journal bytes and drops the pthread/Win32-thread dependency entirely
# (D-BUILD-031). C source; compiles clean on gcc/clang/AppleClang/MSVC.
enable_language(C)
add_library(zstd STATIC ${CMAKE_CURRENT_SOURCE_DIR}/zstd/zstd.c)
add_library(zstd::zstd ALIAS zstd)
target_include_directories(zstd SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/zstd)

# Vulkan-Headers vulkan-sdk-1.4.350.1 — the Vulkan C API declarations
# (m0-rhi-vulkan). Minimal set: vulkan.h + vulkan_core.h + vk_platform.h +
# the vk_video/ std headers vulkan_core.h includes; the C++ bindings
# (vulkan.hpp and friends, ~20 MB) are deliberately NOT vendored — the RHI
# speaks the C API through volk. VK_NO_PROTOTYPES is PUBLIC: nothing may
# link the loader statically; every entry point arrives via volk at runtime,
# which is what makes an ICD-less host a probe result instead of a link error.
add_library(vulkan_headers INTERFACE)
add_library(vulkan::headers ALIAS vulkan_headers)
target_include_directories(vulkan_headers SYSTEM INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/vulkan/include)
target_compile_definitions(vulkan_headers INTERFACE VK_NO_PROTOTYPES)

# volk vulkan-sdk-1.4.350.1 — Vulkan meta-loader (m0-rhi-vulkan). dlopens the
# platform loader at runtime and resolves every entry point through
# vkGetInstanceProcAddr/vkGetDeviceProcAddr; no libvulkan link dependency, so
# the engine binary starts fine on hosts with no Vulkan at all (the RHI
# reports "unavailable" instead of the dynamic linker failing).
enable_language(C)
add_library(volk STATIC volk/volk.c)
add_library(volk::volk ALIAS volk)
target_include_directories(volk SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/volk)
target_link_libraries(volk PUBLIC vulkan::headers)
if(UNIX)
    target_link_libraries(volk PUBLIC ${CMAKE_DL_LIBS})
endif()

# VulkanMemoryAllocator 3.4.0 — GPU memory allocation for the Vulkan backend
# (m0-rhi-vulkan). Header-only upstream; vma_impl.cpp is the one
# VMA_IMPLEMENTATION TU (upstream-documented usage), kept in third_party/ so
# the vendored header body never enters first-party lint. Dynamic Vulkan
# functions only (fetched through volk's vkGet*ProcAddr at device creation).
add_library(vma STATIC vma/vma_impl.cpp)
add_library(vma::vma ALIAS vma)
target_include_directories(vma SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/vma)
target_compile_definitions(vma PUBLIC
    VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1)
target_link_libraries(vma PUBLIC vulkan::headers)
if(NOT MSVC)
    # Allocator bookkeeping, never debugged at -O0; identical in every preset
    # (quickjs/jolt precedent).
    target_compile_options(vma PRIVATE -O2)
endif()

# glslang vulkan-sdk-1.4.350.1 — GLSL -> SPIR-V compilation (m0-rhi-vulkan,
# spec section 5: "compiled via glslang (BSD) to SPIR-V"). Minimal source
# set: MachineIndependent + preprocessor + GenericCodeGen + OSDependent +
# ResourceLimits + SPIRV, C++ API only — no HLSL front-end (ENABLE_HLSL off,
# upstream-deprecated), no C interface TUs, no SPIRV-Tools optimizer
# (ENABLE_OPT off), no StandAlone tools. glslang/build_info.h is GENERATED
# once at vendor time with upstream's build_info.py (the one non-pristine
# file, like zstd's amalgamation — recorded in the manifest role).
# Deterministic FP applies: GLSL constant folding happens in this library,
# and SPIR-V bytes must not depend on the host toolchain's FP mood.
add_library(glslang_vendored STATIC
    glslang/glslang/MachineIndependent/attribute.cpp
    glslang/glslang/MachineIndependent/Constant.cpp
    glslang/glslang/MachineIndependent/glslang_tab.cpp
    glslang/glslang/MachineIndependent/InfoSink.cpp
    glslang/glslang/MachineIndependent/Initialize.cpp
    glslang/glslang/MachineIndependent/Intermediate.cpp
    glslang/glslang/MachineIndependent/intermOut.cpp
    glslang/glslang/MachineIndependent/IntermTraverse.cpp
    glslang/glslang/MachineIndependent/iomapper.cpp
    glslang/glslang/MachineIndependent/limits.cpp
    glslang/glslang/MachineIndependent/linkValidate.cpp
    glslang/glslang/MachineIndependent/parseConst.cpp
    glslang/glslang/MachineIndependent/ParseContextBase.cpp
    glslang/glslang/MachineIndependent/ParseHelper.cpp
    glslang/glslang/MachineIndependent/PoolAlloc.cpp
    glslang/glslang/MachineIndependent/propagateNoContraction.cpp
    glslang/glslang/MachineIndependent/reflection.cpp
    glslang/glslang/MachineIndependent/RemoveTree.cpp
    glslang/glslang/MachineIndependent/Scan.cpp
    glslang/glslang/MachineIndependent/ShaderLang.cpp
    glslang/glslang/MachineIndependent/SpirvIntrinsics.cpp
    glslang/glslang/MachineIndependent/SymbolTable.cpp
    glslang/glslang/MachineIndependent/Versions.cpp
    glslang/glslang/MachineIndependent/preprocessor/Pp.cpp
    glslang/glslang/MachineIndependent/preprocessor/PpAtom.cpp
    glslang/glslang/MachineIndependent/preprocessor/PpContext.cpp
    glslang/glslang/MachineIndependent/preprocessor/PpScanner.cpp
    glslang/glslang/MachineIndependent/preprocessor/PpTokens.cpp
    glslang/glslang/GenericCodeGen/CodeGen.cpp
    glslang/glslang/GenericCodeGen/Link.cpp
    glslang/glslang/ResourceLimits/ResourceLimits.cpp
    glslang/SPIRV/GlslangToSpv.cpp
    glslang/SPIRV/InReadableOrder.cpp
    glslang/SPIRV/Logger.cpp
    glslang/SPIRV/SpvBuilder.cpp
    glslang/SPIRV/SpvPostProcess.cpp
    glslang/SPIRV/SpvTools.cpp
    glslang/SPIRV/doc.cpp
    glslang/SPIRV/disassemble.cpp
)
if(WIN32)
    target_sources(glslang_vendored PRIVATE glslang/glslang/OSDependent/Windows/ossource.cpp)
    target_compile_definitions(glslang_vendored PRIVATE GLSLANG_OSINCLUDE_WIN32)
else()
    target_sources(glslang_vendored PRIVATE glslang/glslang/OSDependent/Unix/ossource.cpp)
    target_compile_definitions(glslang_vendored PRIVATE GLSLANG_OSINCLUDE_UNIX)
endif()
add_library(glslang::glslang ALIAS glslang_vendored)
target_include_directories(glslang_vendored SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/glslang)
target_compile_definitions(glslang_vendored PUBLIC ENABLE_SPIRV)
find_package(Threads QUIET)
if(TARGET Threads::Threads)
    target_link_libraries(glslang_vendored PUBLIC Threads::Threads)
endif()
if(NOT MSVC)
    # Parser tables + SPIR-V builder: never debugged at -O0, identical in
    # every preset (quickjs/jolt/ryml precedent).
    target_compile_options(glslang_vendored PRIVATE -O2)
endif()
midday_apply_deterministic_fp(glslang_vendored)

# stb_image_write (stb master 31c1ad37) — PNG encoding for render-to-image
# transport (m0-rhi-vulkan). Golden hashes are DECODED PIXELS, never these
# encoded bytes (Aurora D-14); the PNG is the human-inspectable artifact.
# stb_image_write_impl.c is the one STB_IMAGE_WRITE_IMPLEMENTATION TU
# (upstream-documented usage), kept in third_party/ like vma_impl.cpp.
enable_language(C)
add_library(stb_image_write STATIC stb/stb_image_write_impl.c)
add_library(stb::image_write ALIAS stb_image_write)
target_include_directories(stb_image_write SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/stb)

# rapidyaml (ryml) 0.15.2 — strict-YAML parsing for the PERMANENT loader
# subset (core/loader, m0-yaml-loader-run). Amalgamated single header, kept
# byte-pristine; ryml.cpp is the one TU defining RYML_SINGLE_HDR_DEFINE_NOW
# (upstream convention). The loader wrapper (core/loader/yaml_parse.cpp) is
# the ONLY first-party consumer: it converts the ryml tree into an owned
# first-party node model and layers the strictness ryml deliberately leaves
# to callers (duplicate keys, anchors/aliases/tags, multi-doc refusal).
# Parse-only text handling — no float formatting reaches sim state (scalar
# conversion goes through fast_float in the wrapper) — but the FP contract
# applies uniformly like every vendored sim-adjacent target.
add_library(ryml STATIC ${CMAKE_CURRENT_SOURCE_DIR}/ryml/ryml.cpp)
add_library(ryml::ryml ALIAS ryml)
target_include_directories(ryml SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/ryml)
if(NOT MSVC)
    # Same class as quickjs/jolt: never debugged at -O0, identical in every preset.
    target_compile_options(ryml PRIVATE -O2)
endif()
midday_apply_deterministic_fp(ryml)
