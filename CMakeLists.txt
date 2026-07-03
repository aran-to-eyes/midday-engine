# Midday Engine — top-level build (CMake >= 3.27 + Ninja, spec section 14).
# Tiered layering: core/ compiles rarely; agents never need a C++ build.

cmake_minimum_required(VERSION 3.27)

project(midday
    VERSION 0.1.0
    DESCRIPTION "Midday Engine — AI-first open source 3D game engine"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # always on (spec section 14)

option(MIDDAY_WERROR "Treat warnings as errors (CI lanes)" OFF)
set(MIDDAY_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. 'address,undefined'")

include(cmake/DeterministicFP.cmake)

# Common compile options shared by every first-party target.
add_library(midday_options INTERFACE)
add_library(midday::options ALIAS midday_options)
if(MSVC)
    # /Zc:preprocessor: the conformant preprocessor. The traditional one
    # garbles raw string literals stringized inside macros (doctest CHECK),
    # producing bogus "illegal escape sequence" errors (D-BUILD-016).
    target_compile_options(midday_options INTERFACE /W4 /permissive- /utf-8 /Zc:preprocessor)
    if(MIDDAY_WERROR)
        target_compile_options(midday_options INTERFACE /WX)
    endif()
else()
    target_compile_options(midday_options INTERFACE -Wall -Wextra -Wpedantic)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # gcc's -Wmaybe-uninitialized misfires at -O3 inside std::variant move
        # machinery whenever reflect descriptors are built with Json defaults
        # (hit core/expr and api/ within two nodes; a per-TU pragma does not
        # scale to a pattern every registration TU repeats). Known-unreliable
        # diagnostic on variant-heavy code; clang, MSVC, ASan/UBSan still
        # cover real uninitialized reads (D-BUILD-043).
        target_compile_options(midday_options INTERFACE -Wno-maybe-uninitialized)
    endif()
    if(MIDDAY_WERROR)
        target_compile_options(midday_options INTERFACE -Werror)
    endif()
endif()
if(MIDDAY_SANITIZE)
    if(MSVC)
        # MSVC supports ASan only; UBSan is a Clang/GCC lane concern.
        target_compile_options(midday_options INTERFACE /fsanitize=address)
    else()
        target_compile_options(midday_options INTERFACE
            -fsanitize=${MIDDAY_SANITIZE} -fno-omit-frame-pointer)
        target_link_options(midday_options INTERFACE -fsanitize=${MIDDAY_SANITIZE})
    endif()
endif()

# First-party include root: headers are addressed repo-relative ("cli/json.h").
target_include_directories(midday_options INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(third_party)
add_subdirectory(core)
add_subdirectory(api)
add_subdirectory(cli)
