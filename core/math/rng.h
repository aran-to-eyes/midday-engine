// core/math/rng.h — seeded, counter-based, stream-splittable RNG.
//
// Generator: Philox4x32-10 (Salmon et al., "Parallel Random Numbers: As Easy
// as 1, 2, 3", SC'11 — the Random123 family; also NumPy's and JAX's
// counter-based generator). Chosen over PCG because Philox is a PURE function
// of (key, counter): no sequential hidden state, O(1) jump to any position,
// and stream identity is just more counter bits — exactly the shape the
// determinism contract wants (see D-BUILD-017). Integer-only: bit-exact on
// every platform. Verified against the official Random123 known-answer
// vectors in math.rng tests.
//
// State layout (documented so journals can serialize it later):
//   key      = 64-bit seed            (Philox key words k0 = lo32, k1 = hi32)
//   counter  = [block lo32, block hi32, stream lo32, stream hi32]
// Each 128-bit counter block yields four uint32 draws; the stream buffers
// them and increments the 64-bit block index.
//
// Stream splitting (order-independent, NO draw consumed from the parent):
//   child(name)  -> stream' = XXH3-64("N" || parent stream || name id)
//   child(index) -> stream' = XXH3-64("I" || parent stream || index)
// Distinct (seed, stream) pairs index disjoint counter blocks by
// construction; the domain-tag byte keeps name- and index-derived children
// from colliding. There is NO global RNG state anywhere in the engine.
//
// Determinism classes:
//   * draws + uniform int/float/double: integer-only or exact float scaling —
//     BIT-PORTABLE.
//   * normal(): inverse-CDF (Acklam's rational approximation, |rel err| <
//     1.15e-9 — far below float32 resolution) over a 53-bit uniform. The tail
//     needs ln(); libm is NOT bit-portable, so it uses the deterministic
//     det_log() below — BIT-PORTABLE.
//   * disk/sphere sampling: rejection methods (Marsaglia 1972) — sqrt only,
//     BIT-PORTABLE. Draw COUNT varies per sample; the stream state stays
//     deterministic because rejection consumes draws through the same
//     counter sequence.

#pragma once

#include "core/base/name.h"
#include "core/math/vec.h"

#include <cstdint>

namespace midday::math {

// Deterministic natural log: |rel err| < 1e-14 over normal doubles, bit-exact
// across platforms (exponent/mantissa split + fixed-order atanh series; only
// IEEE +,-,*,/ — no libm). Domain: x > 0 and finite; out-of-domain input is
// the caller's bug (normal() only ever passes (0, 1)).
double det_log(double x);

namespace detail {

// The raw Philox4x32-10 block function — exposed so math.rng tests can pin
// the official Random123 known-answer vectors against it directly.
struct PhiloxOut {
    std::uint32_t w[4];
};

PhiloxOut philox4x32_10(std::uint32_t c0,
                        std::uint32_t c1,
                        std::uint32_t c2,
                        std::uint32_t c3,
                        std::uint32_t k0,
                        std::uint32_t k1);

} // namespace detail

class RngStream {
public:
    explicit RngStream(std::uint64_t seed, std::uint64_t stream = 0)
        : seed_(seed), stream_(stream) {}

    [[nodiscard]] std::uint64_t seed() const { return seed_; }

    [[nodiscard]] std::uint64_t stream() const { return stream_; }

    // Child streams: pure derivations, order-independent, parent untouched.
    [[nodiscard]] RngStream child(const base::Name& name) const;

    [[nodiscard]] RngStream child(std::uint64_t index) const;

    std::uint32_t next_u32();

    // Two u32 draws, low word first.
    std::uint64_t next_u64();

    // [0, 1), 24-bit resolution: (u32 >> 8) * 2^-24. Exact scaling.
    float uniform_float();

    // [0, 1), 53-bit resolution: (u64 >> 11) * 2^-53. Exact scaling.
    double uniform_double();

    // [0, bound) unbiased — Lemire's multiply-with-rejection ("Fast Random
    // Integer Generation in an Interval", 2019). bound >= 1.
    std::uint32_t uniform_below(std::uint32_t bound);

    // [lo, hi] inclusive, unbiased; requires lo <= hi. Spans up to the full
    // int32 range.
    std::int32_t uniform_int(std::int32_t lo, std::int32_t hi);

    // [0, bound) unbiased for 64-bit bounds — bitmask rejection (no 128-bit
    // multiply, so no compiler-specific intrinsics). bound >= 1.
    std::uint64_t uniform_u64_below(std::uint64_t bound);

    // Standard normal (mean 0, sd 1), inverse-CDF method — bit-portable.
    float normal();

    // Uniform on the unit circle boundary's interior: |v| <= 1 (rejection).
    Vec2 in_disk();

    // Uniform on the unit sphere SURFACE (Marsaglia 1972 — sqrt only).
    Vec3 on_sphere();

    // Uniform in the unit ball: |v| <= 1 (rejection).
    Vec3 in_sphere();

private:
    void refill();

    std::uint64_t seed_;
    std::uint64_t stream_;
    std::uint64_t block_ = 0;
    std::uint32_t buffer_[4] = {};
    std::uint32_t buffered_ = 0; // draws remaining in buffer_ (consumed from index 4 - buffered_)
};

} // namespace midday::math
