// core/base/float_format.cpp — float_format.h. The general-style placement
// logic is shared by the float64 and float32 paths (append_general): the ONLY
// difference between them is which dragonbox overload produces the shortest
// significand, so both inherit the same std::to_chars(general)-equivalence
// proof. Integer tokens go through std::to_chars<integer>, which IS universal
// and locale-free (only the FP overload is the portability problem).

#include "core/base/float_format.h"

#include "dragonbox.h"

#include <bit>
#include <cassert>
#include <charconv>
#include <cmath>
#include <cstdint>

namespace midday::base {
namespace {

// Exact decimal digits of an integer-valued finite double (|value| < 1e22 by
// construction: beyond that, scientific is always shorter). Every float32 is
// exactly representable as a double, so the float32 path reaches this with an
// exact magnitude too. Portable — no __int128 (MSVC) and no FP to_chars (old
// libc++): mantissa digits doubled exponent times, at most 21 doublings of a
// <= 23-digit decimal string.
void append_exact_integer_digits(std::string& out, double magnitude) {
    if (magnitude < 9007199254740992.0) { // < 2^53: exact in uint64 directly
        char buf[24];
        const auto [end, ec] =
            std::to_chars(buf, buf + sizeof buf, static_cast<std::uint64_t>(magnitude));
        assert(ec == std::errc{});
        out.append(buf, end);
        return;
    }
    const auto bits = std::bit_cast<std::uint64_t>(magnitude);
    const int e2 = static_cast<int>((bits >> 52) & 0x7FF) - 1023 - 52;
    const std::uint64_t mantissa = (bits & 0xFFFFFFFFFFFFFULL) | (1ULL << 52);
    assert(e2 > 0);  // >= 2^53 and integer-valued implies a positive shift
    char digits[32]; // most significant first; < 1e22 -> at most 22 digits
    const auto [mant_end, ec] = std::to_chars(digits, digits + sizeof digits, mantissa);
    assert(ec == std::errc{});
    int len = static_cast<int>(mant_end - digits);
    for (int step = 0; step < e2; ++step) { // digits *= 2, in decimal
        int carry = 0;
        for (int i = len - 1; i >= 0; --i) {
            const int doubled = (digits[i] - '0') * 2 + carry;
            digits[i] = static_cast<char>('0' + doubled % 10);
            carry = doubled / 10;
        }
        if (carry != 0) {
            for (int i = len; i > 0; --i)
                digits[i] = digits[i - 1];
            digits[0] = static_cast<char>('0' + carry);
            ++len;
        }
    }
    out.append(digits, digits + len);
}

// The std::to_chars(general) style placement of one shortest decimal
// (`significand` * 10^`dec_exponent`, sign `is_negative`) onto `out`.
// `magnitude` is |value| as a double (exact for float32 inputs) for the
// exact-integer expansion. `UInt` is dragonbox's carrier: uint64 for double,
// uint32 for float — the ONLY thing that differs between the two callers.
template <class UInt>
void append_general(
    std::string& out, bool is_negative, UInt significand, int dec_exponent, double magnitude) {
    if (is_negative)
        out += '-';
    char digits[24];
    const auto [digits_end, ec] = std::to_chars(digits, digits + sizeof digits, significand);
    assert(ec == std::errc{});
    const int ndigits = static_cast<int>(digits_end - digits);
    const int x = ndigits - 1 + dec_exponent; // power-of-ten (scientific) exponent

    // std::to_chars(general) style choice: shorter form wins, ties go fixed.
    const int exp_digits = x <= -100 || x >= 100 ? 3 : 2;
    const int sci_len = (ndigits == 1 ? 1 : ndigits + 1) + 2 + exp_digits;
    const int fixed_len = x >= ndigits - 1 ? x + 1 : (x >= 0 ? ndigits + 1 : ndigits + 1 - x);

    if (fixed_len <= sci_len) {
        if (x >= ndigits - 1) {
            // Integer with no fractional part: the EXACT expansion (zero-padded
            // shortest digits would round-trip but differ from the pinned bytes).
            append_exact_integer_digits(out, magnitude);
        } else if (x >= 0) { // decimal point inside the digits
            out.append(digits, digits + x + 1);
            out += '.';
            out.append(digits + x + 1, digits_end);
        } else { // 0.000ddd
            out += "0.";
            out.append(static_cast<std::size_t>(-x - 1), '0');
            out.append(digits, digits_end);
        }
    } else { // d[.ddd]e±XX
        out += digits[0];
        if (ndigits > 1) {
            out += '.';
            out.append(digits + 1, digits_end);
        }
        out += 'e';
        out += x < 0 ? '-' : '+';
        char exp_buf[8];
        const auto [exp_end, exp_ec] =
            std::to_chars(exp_buf, exp_buf + sizeof exp_buf, x < 0 ? -x : x);
        assert(exp_ec == std::errc{});
        if (exp_end - exp_buf < 2)
            out += '0'; // exponents are at least two digits: 1e+05
        out.append(exp_buf, exp_end);
    }
}

} // namespace

void append_shortest_double(std::string& out, double value) {
    // Non-finite is the caller's concern (json.h serializes it as null).
    if (value == 0.0) {
        if (std::signbit(value))
            out += '-';
        out += '0';
        return;
    }
    const auto dec = jkj::dragonbox::to_decimal(value); // unique shortest binary64 digits
    append_general(out, dec.is_negative, dec.significand, dec.exponent, std::abs(value));
}

void append_shortest_float(std::string& out, float value) {
    if (value == 0.0F) {
        if (std::signbit(value))
            out += '-';
        out += '0';
        return;
    }
    const auto dec = jkj::dragonbox::to_decimal(value); // unique shortest binary32 digits
    append_general(
        out, dec.is_negative, dec.significand, dec.exponent, std::abs(static_cast<double>(value)));
}

} // namespace midday::base
