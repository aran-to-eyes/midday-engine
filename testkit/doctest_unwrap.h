// testkit/doctest_unwrap.h — assert-and-access for std::optional in selftests
// (shared across subsystem test OBJECT libs; never compiled into libraries).
// REQUIRE aborts the test case when empty; the (unreachable) abort() makes
// every subsequent access provably checked for the
// bugprone-unchecked-optional-access dataflow.

#pragma once

#include "testkit/doctest.h"

#include <cstdlib>
#include <optional>

namespace midday::testkit {

template <typename T> T& unwrap(std::optional<T>& opt) {
    REQUIRE(opt.has_value());
    if (!opt.has_value())
        std::abort(); // unreachable: REQUIRE threw
    return *opt;
}

template <typename T> const T& unwrap(const std::optional<T>& opt) {
    REQUIRE(opt.has_value());
    if (!opt.has_value())
        std::abort(); // unreachable: REQUIRE threw
    return *opt;
}

} // namespace midday::testkit
