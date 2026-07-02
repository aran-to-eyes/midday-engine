# iOS arm64 (device). Compile lane activates at m1-xplat; execution at M7
# (Meridian D18 staging).
# Usage: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/ios-arm64.cmake ...

set(CMAKE_SYSTEM_NAME iOS)
set(CMAKE_SYSTEM_PROCESSOR arm64)
set(CMAKE_OSX_ARCHITECTURES arm64)
set(CMAKE_OSX_SYSROOT iphoneos)
# Metal 3 baseline, aligned with the macOS deployment floor.
set(CMAKE_OSX_DEPLOYMENT_TARGET "16.0" CACHE STRING "Minimum iOS version")

if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    message(FATAL_ERROR
        "ios-arm64: building for iOS requires a macOS host with Xcode "
        "(Apple SDKs are not redistributable).")
endif()

# Static-library try_compile: no signing/entitlements needed at configure time.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
