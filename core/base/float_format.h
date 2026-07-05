// core/base/float_format.h — the ONE portable, deterministic FP->string
// primitive: the shortest round-trip decimal of a float64 / float32, emitted
// with the std::to_chars(general) style rule (fixed or scientific, whichever
// is shorter, ties to fixed).
//
// Both go through vendored dragonbox for the shortest significand digits and
// share the identical general-style placement logic — so the bytes are
// byte-identical to std::to_chars<double>/<float> yet FREE of the FP
// std::to_chars overload entirely. That overload is not universally
// available: the macOS-14 libc++ has no FP to_chars at all (D-BUILD-015,
// which established this policy for doubles in json_write.cpp), and the iOS
// SDK marks std::to_chars<float> "introduced in iOS 16.3" — so any code that
// touched it broke the build-ios lane below its 16.0 deployment floor. This
// module removes that dependency for float32 exactly as json_write.cpp
// already did for doubles.
//
// dragonbox has a native binary32 path (`to_decimal(float)`), so
// append_shortest_float yields the shortest decimal of the FLOAT32 value
// (e.g. 1.2f -> "1.2"), never the widened-double spelling
// ("1.2000000476837158").
//
// Contract: both APPEND to `out`; both handle signed zero ("-0"/"0"); a
// non-finite value is the CALLER's concern (dragonbox asserts finite) —
// JSON serializes it as null (json.h), and every float32 the loader emits is
// finite by construction. Validated byte-for-byte against std::to_chars over
// a full-float32-domain sweep + a double cross-check (float_format_test.cpp,
// the append_exact_integer_digits / general-style corpus), the same
// methodology the double path used across 64.6M doubles.

#pragma once

#include <string>

namespace midday::base {

void append_shortest_double(std::string& out, double value);
void append_shortest_float(std::string& out, float value);

} // namespace midday::base
