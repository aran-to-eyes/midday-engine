#include "cli/verb.h"

#ifndef MIDDAY_VERSION
#define MIDDAY_VERSION "0.0.0-unversioned"
#endif

namespace midday::cli {
namespace {

constexpr std::string_view platform() {
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

constexpr std::string_view arch() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#else
    return "unknown";
#endif
}

} // namespace

VerbOutcome verb_version(const VerbArgs&) {
    VerbOutcome out;
    out.payload.set("name", "midday");
    out.payload.set("version", MIDDAY_VERSION);
    out.payload.set("platform", platform());
    out.payload.set("arch", arch());
    out.human = std::string("midday ") + MIDDAY_VERSION + " (" + std::string(platform()) + "-" +
                std::string(arch()) + ")";
    return out;
}

} // namespace midday::cli
