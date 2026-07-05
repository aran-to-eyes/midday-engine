// base.float_format — the portable shortest-decimal FP formatter. Known-answer
// pins ONLY (no std::to_chars<float> anywhere: that overload is the very thing
// this module removes, and it must compile on iOS < 16.3). The expected bytes
// are what std::to_chars<double>/<float> (general) produces — cross-checked
// once locally against std::to_chars over the full finite float32 domain +
// the double corpus, the same methodology json_write's double path used.

#include "core/base/float_format.h"
#include "testkit/doctest.h"

#include <string>

using namespace midday;

namespace {

std::string flt(float value) {
    std::string out;
    base::append_shortest_float(out, value);
    return out;
}

std::string dbl(double value) {
    std::string out;
    base::append_shortest_double(out, value);
    return out;
}

} // namespace

TEST_CASE("base.float_format: float32 shortest, NOT the widened-double spelling") {
    // The bug this module's caller (machine_emit) exists to avoid: 1.2f
    // widened to double is 1.2000000476837158 — the float32-shortest is "1.2".
    CHECK(flt(1.2F) == "1.2");
    CHECK(flt(4.5F) == "4.5");
    CHECK(flt(0.30F) == "0.3");
    CHECK(flt(0.6F) == "0.6");
    CHECK(flt(1.4F) == "1.4");
    CHECK(flt(0.25F) == "0.25"); // exact in binary32
    CHECK(flt(0.9F) == "0.9");
    CHECK(flt(2.4F) == "2.4");
    CHECK(flt(4.55F) == "4.55");
    CHECK(flt(0.75F) == "0.75");
}

TEST_CASE("base.float_format: the Warden fixture float values round-trip verbatim") {
    // Every float literal the canonical machine emitter serializes from
    // examples/warden/ (sequence times, Vec3 translations, component sizes).
    CHECK(flt(0.0F) == "0");
    CHECK(flt(1.0F) == "1");
    CHECK(flt(2.0F) == "2");
    CHECK(flt(1.2F) == "1.2");
    CHECK(flt(1.4F) == "1.4");
    CHECK(flt(4.5F) == "4.5");
    CHECK(flt(0.4F) == "0.4");
    CHECK(flt(0.8F) == "0.8");
    CHECK(flt(0.3F) == "0.3");
    CHECK(flt(0.6F) == "0.6");
    CHECK(flt(0.25F) == "0.25");
    CHECK(flt(3.2F) == "3.2");
    CHECK(flt(0.9F) == "0.9");
    CHECK(flt(18.0F) == "18");
    CHECK(flt(110.0F) == "110");
    CHECK(flt(120.0F) == "120");
}

TEST_CASE("base.float_format: signed zero and negatives") {
    CHECK(flt(-0.0F) == "-0");
    CHECK(flt(0.0F) == "0");
    CHECK(flt(-1.2F) == "-1.2");
    CHECK(flt(-6.0F) == "-6");
    CHECK(flt(-10.0F) == "-10");
}

TEST_CASE("base.float_format: the fixed/scientific general-style boundary") {
    // Ties go fixed; otherwise the shorter form wins — identical rule to the
    // double path (api/CODEGEN.md "Numbers").
    CHECK(flt(1e-04F) == "1e-04");  // sci (5) beats fixed "0.0001" (6)
    CHECK(flt(0.001F) == "0.001");  // tie -> fixed
    CHECK(flt(0.0001F) == "1e-04"); // same value as 1e-04F
    CHECK(flt(100000.0F) == "1e+05");
    CHECK(flt(1000000.0F) == "1e+06");
    CHECK(flt(12.5F) == "12.5");
    CHECK(flt(1234.5F) == "1234.5");
    CHECK(flt(1e20F) == "1e+20");
    CHECK(flt(1.5e-10F) == "1.5e-10"); // three-digit-exponent boundary is 100
}

TEST_CASE("base.float_format: integer-valued floats expand exactly, not zero-padded") {
    CHECK(flt(16777216.0F) == "16777216"); // 2^24, exact in binary32
    CHECK(flt(255.0F) == "255");
    CHECK(flt(1024.0F) == "1024");
}

TEST_CASE("base.float_format: append_shortest_double matches the JSON writer's pinned bytes") {
    // The double path moved into this shared primitive — it must still produce
    // the bytes json_write.cpp's corpus pins.
    CHECK(dbl(0.0) == "0");
    CHECK(dbl(-0.0) == "-0");
    CHECK(dbl(1.2) == "1.2");
    CHECK(dbl(0.0001) == "1e-04");
    CHECK(dbl(1.0) == "1");
    CHECK(dbl(9223372036854775808.0) == "9223372036854775808"); // 2^63, exact expansion
    CHECK(dbl(1e21) == "1e+21");
}
