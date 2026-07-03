#include "core/math/rng.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <bit>
#include <cmath>
#include <cstring>
#include <numbers>

namespace midday::math {

// ---- Philox4x32-10 (Salmon et al. 2011, Random123 reference constants) ----

namespace {

constexpr std::uint32_t kPhiloxM0 = 0xD2511F53u;
constexpr std::uint32_t kPhiloxM1 = 0xCD9E8D57u;
constexpr std::uint32_t kPhiloxW0 = 0x9E3779B9u; // golden ratio
constexpr std::uint32_t kPhiloxW1 = 0xBB67AE85u; // sqrt(3) - 1

} // namespace

namespace detail {

PhiloxOut philox4x32_10(std::uint32_t c0,
                        std::uint32_t c1,
                        std::uint32_t c2,
                        std::uint32_t c3,
                        std::uint32_t k0,
                        std::uint32_t k1) {
    for (int round = 0; round < 10; ++round) {
        if (round > 0) {
            k0 += kPhiloxW0;
            k1 += kPhiloxW1;
        }
        const std::uint64_t p0 = std::uint64_t{kPhiloxM0} * c0;
        const std::uint64_t p1 = std::uint64_t{kPhiloxM1} * c2;
        const std::uint32_t n0 = static_cast<std::uint32_t>(p1 >> 32) ^ c1 ^ k0;
        const auto n1 = static_cast<std::uint32_t>(p1);
        const std::uint32_t n2 = static_cast<std::uint32_t>(p0 >> 32) ^ c3 ^ k1;
        const auto n3 = static_cast<std::uint32_t>(p0);
        c0 = n0;
        c1 = n1;
        c2 = n2;
        c3 = n3;
    }
    return {{c0, c1, c2, c3}};
}

} // namespace detail

namespace {

// Child-stream derivation: XXH3-64 over a fixed-layout little-endian message
// (domain tag byte, parent stream, child key). Pure, order-independent.
std::uint64_t derive_stream(char tag, std::uint64_t parent_stream, std::uint64_t key) {
    unsigned char msg[17];
    msg[0] = static_cast<unsigned char>(tag);
    for (int i = 0; i < 8; ++i) {
        msg[1 + i] = static_cast<unsigned char>(parent_stream >> (8 * i));
        msg[9 + i] = static_cast<unsigned char>(key >> (8 * i));
    }
    return XXH3_64bits(msg, sizeof msg);
}

// ---- Acklam's inverse normal CDF (rational approximation, 2003) ----
// |relative error| < 1.15e-9 over (0, 1) — invisible at float32 output.
// Central region is pure rational arithmetic; tails add det_log + sqrt.
// All coefficients are Acklam's published values.

constexpr double kIcdfA[6] = {-3.969683028665376e+01,
                              2.209460984245205e+02,
                              -2.759285104469687e+02,
                              1.383577518672690e+02,
                              -3.066479806614716e+01,
                              2.506628277459239e+00};
constexpr double kIcdfB[5] = {-5.447609879822406e+01,
                              1.615858368580409e+02,
                              -1.556989798598866e+02,
                              6.680131188771972e+01,
                              -1.328068155288572e+01};
constexpr double kIcdfC[6] = {-7.784894002430293e-03,
                              -3.223964580411365e-01,
                              -2.400758277161838e+00,
                              -2.549732539343734e+00,
                              4.374664141464968e+00,
                              2.938163982698783e+00};
constexpr double kIcdfD[4] = {
    7.784695709041462e-03, 3.224671290700398e-01, 2.445134137142996e+00, 3.754408661907416e+00};
constexpr double kIcdfPLow = 0.02425;

double icdf_tail(double p) { // p in (0, kIcdfPLow): returns the NEGATIVE tail value
    const double q = std::sqrt(-2.0 * det_log(p));
    return (((((kIcdfC[0] * q + kIcdfC[1]) * q + kIcdfC[2]) * q + kIcdfC[3]) * q + kIcdfC[4]) * q +
            kIcdfC[5]) /
           ((((kIcdfD[0] * q + kIcdfD[1]) * q + kIcdfD[2]) * q + kIcdfD[3]) * q + 1.0);
}

double inverse_normal_cdf(double p) { // p in (0, 1)
    if (p < kIcdfPLow)
        return icdf_tail(p);
    if (p > 1.0 - kIcdfPLow)
        return -icdf_tail(1.0 - p); // 1 - p is exact for p in [0.5, 1)
    const double q = p - 0.5;
    const double r = q * q;
    return (((((kIcdfA[0] * r + kIcdfA[1]) * r + kIcdfA[2]) * r + kIcdfA[3]) * r + kIcdfA[4]) * r +
            kIcdfA[5]) *
           q /
           (((((kIcdfB[0] * r + kIcdfB[1]) * r + kIcdfB[2]) * r + kIcdfB[3]) * r + kIcdfB[4]) * r +
            1.0);
}

} // namespace

double det_log(double x) {
    // Split x = m * 2^e with m in [1, 2): exact bit manipulation.
    std::uint64_t bits = 0;
    std::memcpy(&bits, &x, sizeof bits);
    int e = static_cast<int>((bits >> 52) & 0x7FFu) - 1023;
    if (e == -1023) { // subnormal: rescale by 2^64 (exact), then recurse once
        const double scaled = x * 0x1p64;
        std::memcpy(&bits, &scaled, sizeof bits);
        e = static_cast<int>((bits >> 52) & 0x7FFu) - 1023 - 64;
    }
    const std::uint64_t mantissa = (bits & 0x000FFFFFFFFFFFFFull) | 0x3FF0000000000000ull;
    double m = 0.0;
    std::memcpy(&m, &mantissa, sizeof m);
    // Center the reduction on 1: m in [sqrt(1/2), sqrt(2)) keeps |u| small.
    constexpr double kSqrt2 = std::numbers::sqrt2;
    if (m >= kSqrt2) {
        m *= 0.5;
        e += 1;
    }
    // atanh series: ln(m) = 2u * (1 + u^2/3 + u^4/5 + ... ), u = (m-1)/(m+1).
    // |u| <= 0.1716 -> u^2 <= 0.0295; 11 terms put the truncation error below
    // 1e-16 relative. Fixed Horner order = fixed bits.
    const double u = (m - 1.0) / (m + 1.0);
    const double u2 = u * u;
    double series = 1.0 / 21.0;
    series = series * u2 + 1.0 / 19.0;
    series = series * u2 + 1.0 / 17.0;
    series = series * u2 + 1.0 / 15.0;
    series = series * u2 + 1.0 / 13.0;
    series = series * u2 + 1.0 / 11.0;
    series = series * u2 + 1.0 / 9.0;
    series = series * u2 + 1.0 / 7.0;
    series = series * u2 + 1.0 / 5.0;
    series = series * u2 + 1.0 / 3.0;
    series = series * u2 + 1.0;
    // The classic fdlibm hi/lo SPLIT of ln2 (hi has zeroed low bits so e * hi
    // is exact) — deliberately NOT std::numbers::ln2, which is single-word.
    constexpr double kLn2Hi = 6.93147180369123816490e-01; // NOLINT(modernize-use-std-numbers)
    constexpr double kLn2Lo = 1.90821492927058770002e-10;
    const double log_m = 2.0 * u * series;
    return static_cast<double>(e) * kLn2Hi + (log_m + static_cast<double>(e) * kLn2Lo);
}

RngStream RngStream::child(const base::Name& name) const {
    return RngStream(seed_, derive_stream('N', stream_, name.id()));
}

RngStream RngStream::child(std::uint64_t index) const {
    return RngStream(seed_, derive_stream('I', stream_, index));
}

void RngStream::refill() {
    const detail::PhiloxOut out = detail::philox4x32_10(static_cast<std::uint32_t>(block_),
                                                        static_cast<std::uint32_t>(block_ >> 32),
                                                        static_cast<std::uint32_t>(stream_),
                                                        static_cast<std::uint32_t>(stream_ >> 32),
                                                        static_cast<std::uint32_t>(seed_),
                                                        static_cast<std::uint32_t>(seed_ >> 32));
    for (int i = 0; i < 4; ++i)
        buffer_[i] = out.w[i];
    buffered_ = 4;
    ++block_;
}

std::uint32_t RngStream::next_u32() {
    if (buffered_ == 0)
        refill();
    const std::uint32_t value = buffer_[4 - buffered_];
    --buffered_;
    return value;
}

std::uint64_t RngStream::next_u64() {
    const std::uint64_t lo = next_u32();
    const std::uint64_t hi = next_u32();
    return (hi << 32) | lo;
}

float RngStream::uniform_float() {
    return static_cast<float>(next_u32() >> 8) * 0x1p-24f;
}

double RngStream::uniform_double() {
    return static_cast<double>(next_u64() >> 11) * 0x1p-53;
}

std::uint32_t RngStream::uniform_below(std::uint32_t bound) {
    // Lemire 2019: multiply-shift with rejection of the biased low fringe.
    std::uint64_t m = std::uint64_t{next_u32()} * bound;
    auto low = static_cast<std::uint32_t>(m);
    if (low < bound) {
        const std::uint32_t threshold = (0u - bound) % bound; // 2^32 mod bound
        while (low < threshold) {
            m = std::uint64_t{next_u32()} * bound;
            low = static_cast<std::uint32_t>(m);
        }
    }
    return static_cast<std::uint32_t>(m >> 32);
}

std::int32_t RngStream::uniform_int(std::int32_t lo, std::int32_t hi) {
    const auto span = static_cast<std::uint32_t>(static_cast<std::int64_t>(hi) -
                                                 static_cast<std::int64_t>(lo) + 1);
    if (span == 0) // full int32 range: every u32 maps directly
        return static_cast<std::int32_t>(next_u32() + static_cast<std::uint32_t>(lo));
    return static_cast<std::int32_t>(static_cast<std::int64_t>(lo) + uniform_below(span));
}

std::uint64_t RngStream::uniform_u64_below(std::uint64_t bound) {
    // Bitmask rejection: portable (no 128-bit multiply), unbiased, and the
    // expected draw count is < 2 per sample.
    if (bound <= 1)
        return 0;
    const int bits = std::bit_width(bound - 1);
    const std::uint64_t mask = bits >= 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << bits) - 1);
    std::uint64_t value = next_u64() & mask;
    while (value >= bound)
        value = next_u64() & mask;
    return value;
}

float RngStream::normal() {
    // (draw + 0.5) * 2^-53 lies strictly inside (0, 1): both tails reachable,
    // neither endpoint ever hit. Exact scaling, then the bit-portable ICDF.
    const double p = (static_cast<double>(next_u64() >> 11) + 0.5) * 0x1p-53;
    return static_cast<float>(inverse_normal_cdf(p));
}

Vec2 RngStream::in_disk() {
    for (;;) {
        const Vec2 v{uniform_float() * 2.0f - 1.0f, uniform_float() * 2.0f - 1.0f};
        if (v.length_squared() <= 1.0f)
            return v;
    }
}

Vec3 RngStream::on_sphere() {
    // Marsaglia 1972: (u, v) uniform in the unit disk (excluding the exact
    // center, which cannot map to a unit vector) lifts to the sphere surface.
    for (;;) {
        const float u = uniform_float() * 2.0f - 1.0f;
        const float v = uniform_float() * 2.0f - 1.0f;
        const float s = u * u + v * v;
        if (s >= 1.0f || s == 0.0f)
            continue;
        const float k = 2.0f * std::sqrt(1.0f - s);
        return {u * k, v * k, 1.0f - 2.0f * s};
    }
}

Vec3 RngStream::in_sphere() {
    for (;;) {
        const Vec3 v{uniform_float() * 2.0f - 1.0f,
                     uniform_float() * 2.0f - 1.0f,
                     uniform_float() * 2.0f - 1.0f};
        if (v.length_squared() <= 1.0f)
            return v;
    }
}

} // namespace midday::math
