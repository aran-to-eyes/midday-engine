# Windows x86_64 (MSVC ABI). Native under a Visual Studio environment on the
# build-windows lane; cross-compiles via clang-cl against an xwin-style SDK
# layout. Usage: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/windows-x86_64.cmake ...

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    # Native: let CMake discover cl.exe from the VS developer environment.
    # (build-windows lane enters one via its setup step.)
else()
    # Cross: clang-cl targeting the MSVC ABI. Requires an unpacked Windows
    # SDK + MSVC CRT, e.g. produced by `xwin splat` (MIDDAY_XWIN_ROOT points
    # at its output: crt/ and sdk/ subdirectories).
    if(NOT DEFINED ENV{MIDDAY_XWIN_ROOT})
        message(FATAL_ERROR
            "windows-x86_64: cross-compiling from ${CMAKE_HOST_SYSTEM_NAME} requires "
            "MIDDAY_XWIN_ROOT to point at an xwin splat output (crt/, sdk/).")
    endif()
    set(_xwin "$ENV{MIDDAY_XWIN_ROOT}")
    set(CMAKE_C_COMPILER clang-cl)
    set(CMAKE_CXX_COMPILER clang-cl)
    set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
    set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)
    set(CMAKE_LINKER lld-link)
    string(CONCAT _win_flags
        " /winsdkdir ${_xwin}/sdk"
        " /vctoolsdir ${_xwin}/crt")
    set(CMAKE_C_FLAGS_INIT "${_win_flags}")
    set(CMAKE_CXX_FLAGS_INIT "${_win_flags}")
    set(_win_link_flags
        "-libpath:${_xwin}/crt/lib/x86_64 -libpath:${_xwin}/sdk/lib/um/x86_64 -libpath:${_xwin}/sdk/lib/ucrt/x86_64")
    set(CMAKE_EXE_LINKER_FLAGS_INIT "${_win_link_flags}")
    set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_win_link_flags}")
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
endif()
