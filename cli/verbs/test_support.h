// cli/verbs/test_support.h — shared helpers for verb-level tests, hoisted at
// m0-golden-compare on the second-consumer rule (hex.h / temp_dir.h /
// doctest_unwrap.h precedent; run_test, golden_test, and shot_test each grew
// an identical copy).
//
// invoke() drives a verb through the framework seam (parse_verb_args +
// spec.run) — the exact path main() takes after argv splitting. Parse-level
// usage errors REQUIRE-fail: tests that expect usage outcomes get them from
// the verb itself (unknown op, out-of-range values), never from mistyped
// test tokens. field() is REQUIRE-guarded payload access.

#pragma once

#include "cli/verb.h"
#include "testkit/doctest.h"

#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace midday::cli::testsupport {

inline VerbOutcome invoke(const VerbSpec& spec, const std::vector<std::string>& tokens) {
    Invocation inv;
    inv.verb = spec.name;
    for (const std::string& token : tokens)
        inv.rest.push_back(token);
    ParsedArgs parsed = parse_verb_args(spec, inv);
    REQUIRE_FALSE(parsed.usage.has_value());
    return spec.run(parsed.args);
}

inline const Json& field(const Json& object, std::string_view key) {
    const Json* value = object.find(key);
    REQUIRE(value != nullptr);
    if (value == nullptr)
        std::abort(); // unreachable: REQUIRE threw
    return *value;
}

} // namespace midday::cli::testsupport
