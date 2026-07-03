// core/reflect/fatal.h — internal loud-abort helper. Registry paths never
// throw (contract): invariant violations (duplicate registration, malformed
// descriptors, misused init ladder) print one line and abort, exactly like
// Name collisions (D-BUILD-011). Internal to core/reflect — not API.

#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

namespace midday::reflect::detail {

[[noreturn]] inline void fatal(const std::string& message) {
    std::fprintf(stderr, "midday: fatal: reflect: %s\n", message.c_str());
    std::abort();
}

} // namespace midday::reflect::detail
