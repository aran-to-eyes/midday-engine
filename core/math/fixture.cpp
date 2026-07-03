// The fixture exercises, per iteration (20 counted ops x 50,000 iterations =
// exactly kFixtureOps):
//   RNG draws (u32/float/bounded/normal/sphere), noise (value/perlin/fBm),
//   splines + polynomial easing, TRS compose/decompose, matrix products,
//   point transforms, quaternion nlerp/rotate, and the intersection kernels.
// Only bit-portable operations appear here — see fixture.h for the contract.

#include "core/math/fixture.h"

#include "core/math/ease.h"
#include "core/math/intersect.h"
#include "core/math/mat.h"
#include "core/math/noise.h"
#include "core/math/quat.h"
#include "core/math/rng.h"
#include "core/math/spline.h"
#include "core/math/vec.h"
#include "core/math/xform.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

#include <cstdint>
#include <cstring>

namespace midday::math {
namespace {

constexpr std::uint64_t kFixtureSeed = 0x4D49444441594D30ull; // "MIDDAYM0"
constexpr int kOpsPerIteration = 20;

// Fold values into the digest as explicit little-endian bytes, so the digest
// definition is byte-order-independent (all five target platforms are LE;
// this keeps the definition honest anyway).
class Digest {
public:
    Digest() { XXH3_64bits_reset(&state_); }

    void add(std::uint32_t v) {
        unsigned char bytes[4];
        for (int i = 0; i < 4; ++i)
            bytes[i] = static_cast<unsigned char>(v >> (8 * i));
        XXH3_64bits_update(&state_, bytes, sizeof bytes);
    }

    void add(float f) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof bits);
        add(bits);
    }

    void add(Vec3 v) {
        add(v.x);
        add(v.y);
        add(v.z);
    }

    void add(Quat q) {
        add(q.x);
        add(q.y);
        add(q.z);
        add(q.w);
    }

    [[nodiscard]] std::uint64_t digest() const { return XXH3_64bits_digest(&state_); }

private:
    XXH3_state_t state_{};
};

Vec3 draw_point(RngStream& rng, float radius) {
    return {rng.uniform_float() * 2.0f * radius - radius,
            rng.uniform_float() * 2.0f * radius - radius,
            rng.uniform_float() * 2.0f * radius - radius};
}

} // namespace

std::uint64_t determinism_fixture_hash() {
    static_assert(kFixtureOps % kOpsPerIteration == 0);
    RngStream rng = RngStream(kFixtureSeed).child(std::uint64_t{1});
    Digest digest;

    for (std::uint64_t i = 0; i < kFixtureOps / kOpsPerIteration; ++i) {
        // -- RNG draws (ops 1-6) --
        digest.add(rng.next_u32());
        const float t = rng.uniform_float();
        digest.add(t);
        digest.add(rng.uniform_below(1000003u));
        digest.add(rng.normal());
        const Vec3 dir = rng.on_sphere();
        digest.add(dir);
        const Vec3 point = draw_point(rng, 16.0f);
        digest.add(point);

        // -- noise (ops 7-9) --
        digest.add(value_noise_3d(kFixtureSeed, point));
        digest.add(perlin_3d(kFixtureSeed, point));
        digest.add(fbm_perlin_3d(kFixtureSeed, point, Fbm{3, 2.0f, 0.5f}));

        // -- splines + easing (ops 10-12) --
        const Vec3 p1 = point + dir;
        const Vec3 p2 = point + dir * 2.0f;
        digest.add(bezier_cubic(point, p1, p2, point + dir * 3.0f, t));
        digest.add(catmull_rom(point, p1, p2, point + dir * 3.0f, t));
        digest.add(ease_in_out_bounce(t));

        // -- transforms (ops 13-17) --
        // Rotation from normal draws (never axis-angle: that is libm-bound).
        const Quat q = Quat{rng.normal(), rng.normal(), rng.normal(), rng.normal()}.normalized();
        const Transform trs{point, q, Vec3{0.5f + t, 1.0f, 2.0f - t}};
        const Mat4 m = trs.to_mat4();
        const Transform back = Transform::from_mat4(m);
        digest.add(back.translation);
        digest.add(back.rotation);
        digest.add(back.scale);
        const Mat4 composed = m * Mat4::translation(dir);
        digest.add(composed.transform_point(p1));
        digest.add(m.inverse_affine().transform_point(p2));
        const Quat blended = nlerp(q, Quat::identity(), t);
        digest.add(blended);
        digest.add(blended.rotate(dir));

        // -- intersections (ops 18-20) --
        const Ray ray{point + dir * 8.0f, -dir};
        const Aabb box = Aabb::from_center_extents(point, Vec3{1.5f, 1.0f, 0.5f});
        float t0 = 0.0f;
        float t1 = 0.0f;
        if (ray_aabb(ray, box, t0, t1)) {
            digest.add(t0);
            digest.add(t1);
        }
        float ts = 0.0f;
        if (ray_sphere(ray, Sphere{point, 1.0f + t}, ts))
            digest.add(ts);
        float tt = 0.0f;
        float u = 0.0f;
        float v = 0.0f;
        if (ray_triangle(ray,
                         point + Vec3{-2, -2, 0},
                         point + Vec3{2, -2, 0},
                         point + Vec3{0, 2, 0},
                         tt,
                         u,
                         v)) {
            digest.add(tt);
            digest.add(u);
            digest.add(v);
        }
    }
    return digest.digest();
}

} // namespace midday::math
