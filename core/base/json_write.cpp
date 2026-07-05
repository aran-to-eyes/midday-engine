// Deterministic JSON serialization (see contract in json.h). Migrated from
// cli/json.cpp at m0-core-primitives — the one writer in the tree.
//
// Doubles are emitted via vendored dragonbox (shortest round-trip digits) and
// formatted with the std::to_chars(general) style rule — fixed or scientific,
// whichever is shorter, ties to fixed — so the bytes are identical on every
// platform/toolchain regardless of the standard library's FP support
// (D-BUILD-015; the macos-14 libc++ has no FP to_chars/from_chars at all).
// Validated against std::to_chars over a 64.6M-double cross-check; the
// core.json byte-pin corpus guards it from here on. Integers stay on
// std::to_chars — integer support is universal and locale-free.

#include "core/base/json.h"
#include "dragonbox.h"

#include <bit>
#include <cassert>
#include <charconv>
#include <cmath>
#include <cstdint>

namespace midday::base {
namespace {

void write_escaped(std::string& out, std::string_view s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (c < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                out += "\\u00";
                out += hex[(c >> 4) & 0xf];
                out += hex[c & 0xf];
            } else {
                out += static_cast<char>(c); // UTF-8 passes through untouched
            }
        }
    }
    out += '"';
}

void write_int(std::string& out, std::int64_t value) {
    char buf[32];
    const auto [end, ec] = std::to_chars(buf, buf + sizeof buf, value);
    assert(ec == std::errc{});
    out.append(buf, end);
}

// Exact decimal digits of an integer-valued finite double (|value| < 1e22 by
// construction: beyond that, scientific is always shorter). Portable — no
// __int128 (MSVC) and no FP to_chars (old libc++): mantissa digits doubled
// exponent times, at most 21 doublings of a <= 23-digit decimal string.
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

void write_double(std::string& out, double value) {
    // Non-finite handled by the caller (serializes as null).
    if (value == 0.0) {
        if (std::signbit(value))
            out += '-';
        out += '0';
        return;
    }
    const auto dec = jkj::dragonbox::to_decimal(value); // unique shortest digits
    if (dec.is_negative)
        out += '-';
    char digits[24];
    const auto [digits_end, ec] = std::to_chars(digits, digits + sizeof digits, dec.significand);
    assert(ec == std::errc{});
    const int ndigits = static_cast<int>(digits_end - digits);
    const int x = ndigits - 1 + dec.exponent; // power-of-ten (scientific) exponent

    // std::to_chars(general) style choice: shorter form wins, ties go fixed.
    const int exp_digits = x <= -100 || x >= 100 ? 3 : 2;
    const int sci_len = (ndigits == 1 ? 1 : ndigits + 1) + 2 + exp_digits;
    const int fixed_len = x >= ndigits - 1 ? x + 1 : (x >= 0 ? ndigits + 1 : ndigits + 1 - x);

    if (fixed_len <= sci_len) {
        if (x >= ndigits - 1) {
            // Integer with no fractional part: the EXACT expansion (zero-padded
            // shortest digits would round-trip but differ from the pinned bytes).
            append_exact_integer_digits(out, std::abs(value));
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

bool Json::as_bool() const {
    assert(is_bool());
    return std::get<bool>(value_);
}

std::int64_t Json::as_int() const {
    assert(is_int());
    return std::get<std::int64_t>(value_);
}

double Json::as_double() const {
    assert(is_number());
    if (const auto* i = std::get_if<std::int64_t>(&value_))
        return static_cast<double>(*i);
    return std::get<double>(value_);
}

const std::string& Json::as_string() const {
    assert(is_string());
    return std::get<std::string>(value_);
}

Json& Json::set(std::string_view key, Json value) {
    assert(is_object());
    auto& object = std::get<Object>(value_);
    for (auto& [k, v] : object) {
        if (k == key) {
            v = std::move(value);
            return *this;
        }
    }
    object.emplace_back(std::string(key), std::move(value));
    return *this;
}

const Json* Json::find(std::string_view key) const {
    if (!is_object())
        return nullptr;
    for (const auto& [k, v] : std::get<Object>(value_)) {
        if (k == key)
            return &v;
    }
    return nullptr;
}

Json& Json::push(Json value) {
    assert(is_array());
    std::get<Array>(value_).emplace_back(std::move(value));
    return *this;
}

std::string Json::dump() const {
    std::string out;
    write(out);
    return out;
}

void Json::write(std::string& out) const {
    struct Writer {
        std::string& out;

        void operator()(std::nullptr_t) const { out += "null"; }

        void operator()(bool b) const { out += b ? "true" : "false"; }

        void operator()(std::int64_t n) const { write_int(out, n); }

        void operator()(double d) const {
            if (std::isfinite(d)) {
                write_double(out, d);
            } else {
                out += "null"; // JSON has no NaN/Inf (contract in json.h)
            }
        }

        void operator()(const std::string& s) const { write_escaped(out, s); }

        void operator()(const Array& a) const {
            out += '[';
            for (const Json& v : a) {
                if (out.back() != '[')
                    out += ',';
                v.write(out);
            }
            out += ']';
        }

        void operator()(const Object& o) const {
            out += '{';
            bool first = true;
            for (const auto& [k, v] : o) {
                if (!first)
                    out += ',';
                first = false;
                write_escaped(out, k);
                out += ':';
                v.write(out);
            }
            out += '}';
        }
    };

    std::visit(Writer{out}, value_);
}

} // namespace midday::base
