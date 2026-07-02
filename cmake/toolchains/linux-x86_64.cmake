# Linux x86_64 — the pinned determinism reference platform (spec section 14).
# Native on the Linux x86_64 CI container; cross-compiles via clang elsewhere.
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-x86_64.cmake ...

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux"
   OR NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
    # Cross build: clang with an explicit target triple. A Linux sysroot must
    # be provided (glibc + libstdc++ headers/libs), e.g. one exported from the
    # pinned CI container image.
    set(_triple x86_64-unknown-linux-gnu)
    set(CMAKE_C_COMPILER clang)
    set(CMAKE_CXX_COMPILER clang++)
    set(CMAKE_C_COMPILER_TARGET ${_triple})
    set(CMAKE_CXX_COMPILER_TARGET ${_triple})
    if(DEFINED ENV{MIDDAY_LINUX_SYSROOT})
        set(CMAKE_SYSROOT "$ENV{MIDDAY_LINUX_SYSROOT}")
    else()
        message(FATAL_ERROR
            "linux-x86_64: cross-compiling from ${CMAKE_HOST_SYSTEM_NAME} requires "
            "MIDDAY_LINUX_SYSROOT to point at an x86_64 Linux sysroot.")
    endif()
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
endif()
