# macOS arm64 — hosted macOS lane (never the managed-firewall dev Mac as CI;
# Zenith D027). Usage: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/macos-arm64.cmake ...

set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR arm64)
set(CMAKE_OSX_ARCHITECTURES arm64)
# Metal 3 baseline; raise deliberately, never implicitly.
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum macOS version")

if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    message(FATAL_ERROR
        "macos-arm64: building for macOS requires a macOS host with Xcode "
        "command line tools (Apple SDKs are not redistributable).")
endif()
