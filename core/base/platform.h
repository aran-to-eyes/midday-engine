// core/base/platform.h — compile-time platform identity, one spelling
// tree-wide: `midday version` payloads and journal header info both derive
// from these (never duplicated per consumer). The triple is INFORMATIONAL —
// it must never enter a deterministic hash or byte-compare (journals byte-
// compare across hosts of the SAME platform; cross-platform lanes compare
// semantically).

#pragma once

#include <string>
#include <string_view>

namespace midday::base {

constexpr std::string_view platform_os() {
#if defined(__APPLE__)
    return "macos";
#elif defined(_WIN32)
    return "windows";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

constexpr std::string_view platform_arch() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#else
    return "unknown";
#endif
}

constexpr std::string_view platform_compiler() {
#if defined(_MSC_VER)
    return "msvc";
#elif defined(__apple_build_version__)
    return "appleclang";
#elif defined(__clang__)
    return "clang";
#elif defined(__GNUC__)
    return "gcc";
#else
    return "unknown";
#endif
}

// "os-arch-compiler", e.g. "linux-x86_64-gcc".
inline std::string platform_triple() {
    std::string triple;
    triple.reserve(platform_os().size() + platform_arch().size() + platform_compiler().size() + 2);
    triple.append(platform_os());
    triple.push_back('-');
    triple.append(platform_arch());
    triple.push_back('-');
    triple.append(platform_compiler());
    return triple;
}

} // namespace midday::base
