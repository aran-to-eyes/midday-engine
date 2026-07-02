# Android arm64-v8a. Delegates to the NDK's canonical toolchain file — the
# NDK owns Android compiler/sysroot/STL policy; we pin ABI, API level, and STL.
# Compile lane activates at m1-xplat; execution at M7 (Meridian D18 staging).
# Usage: ANDROID_NDK_ROOT=<ndk> cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/android-arm64.cmake ...

if(NOT DEFINED ENV{ANDROID_NDK_ROOT})
    message(FATAL_ERROR
        "android-arm64: set ANDROID_NDK_ROOT to an Android NDK r26+ install.")
endif()

set(ANDROID_ABI arm64-v8a)
set(ANDROID_PLATFORM android-29)   # Android 10 — Vulkan 1.1 baseline
set(ANDROID_STL c++_static)

include("$ENV{ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake")
