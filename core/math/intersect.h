// core/math/intersect.h — geometric queries: Aabb, Plane, Ray, Sphere and the
// intersection kernels the engine standardizes on:
//   * ray-AABB: slab method (Williams et al. 2005), robust with infinities
//     from axis-parallel rays.
//   * ray-sphere: stable quadratic (geometric b^2 - c form, RTCD section 5.3.2).
//   * ray-triangle: Moller-Trumbore 1997 — the same algorithm Godot's
//     geometry_3d.h uses; epsilon-parallel rejection, no backface culling.
//   * AABB-AABB overlap: closed-interval (touching faces DO overlap) — the
//     conservative choice for broadphase; Godot's open-interval `intersects`
//     is a deliberate deviation, recorded here once.
//   * plane classification: signed distance with caller-supplied epsilon;
//     AABB vs plane via the center-extent projection radius (RTCD 5.2.3).
//
// Conventions:
//   * Aabb is min/max corners (never position+size: no negative-size states).
//   * Plane is n . x = d with n unit length; d is the signed distance of the
//     plane from the origin along n.
//   * Ray directions need NOT be unit length unless a kernel says otherwise;
//     reported t values are in units of the direction's length.
//
// Determinism class: BIT-PORTABLE (arithmetic + sqrt; IEEE inf semantics in
// the slab method are well-defined and identical on every supported target).

#pragma once

#include "core/math/vec.h"

#include <cmath>
#include <cstdint>
#include <limits>

namespace midday::math {

struct Ray {
    Vec3 origin{};
    Vec3 dir{0, 0, 1};

    [[nodiscard]] constexpr Vec3 at(float t) const { return origin + dir * t; }
};

struct Sphere {
    Vec3 center{};
    float radius = 0.0f;
};

struct Aabb {
    Vec3 min{};
    Vec3 max{};

    [[nodiscard]] static constexpr Aabb from_center_extents(Vec3 center, Vec3 extents) {
        return {center - extents, center + extents};
    }

    [[nodiscard]] constexpr Vec3 center() const { return (min + max) * 0.5f; }

    [[nodiscard]] constexpr Vec3 extents() const { return (max - min) * 0.5f; }

    [[nodiscard]] constexpr Vec3 size() const { return max - min; }

    [[nodiscard]] constexpr bool contains(Vec3 p) const {
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z &&
               p.z <= max.z;
    }

    [[nodiscard]] constexpr Aabb merged(const Aabb& other) const {
        return {vec_min(min, other.min), vec_max(max, other.max)};
    }

    [[nodiscard]] constexpr Aabb expanded(Vec3 p) const {
        return {vec_min(min, p), vec_max(max, p)};
    }

    friend constexpr bool operator==(const Aabb& a, const Aabb& b) {
        return a.min == b.min && a.max == b.max;
    }
};

struct Plane {
    Vec3 normal{0, 1, 0}; // unit length by contract
    float d = 0.0f;       // n . x = d

    [[nodiscard]] static Plane from_point_normal(Vec3 point, Vec3 unit_normal) {
        return {unit_normal, dot(unit_normal, point)};
    }

    // Counter-clockwise winding (right-handed) yields the front-facing normal.
    [[nodiscard]] static Plane from_points(Vec3 a, Vec3 b, Vec3 c) {
        const Vec3 n = cross(b - a, c - a).normalized();
        return {n, dot(n, a)};
    }

    [[nodiscard]] constexpr float signed_distance(Vec3 p) const { return dot(normal, p) - d; }
};

enum class PlaneSide : std::uint8_t { On, Front, Back };

constexpr PlaneSide classify(const Plane& plane, Vec3 point, float eps = 1e-5f) {
    const float dist = plane.signed_distance(point);
    if (dist > eps)
        return PlaneSide::Front;
    if (dist < -eps)
        return PlaneSide::Back;
    return PlaneSide::On;
}

// AABB vs plane: project the box's extents onto the normal (RTCD 5.2.3);
// `On` means straddling/touching.
constexpr PlaneSide classify(const Plane& plane, const Aabb& box) {
    const float dist = plane.signed_distance(box.center());
    const Vec3 n = vec_abs(plane.normal);
    const float radius = dot(box.extents(), n);
    if (dist > radius)
        return PlaneSide::Front;
    if (dist < -radius)
        return PlaneSide::Back;
    return PlaneSide::On;
}

// Closed-interval overlap: touching faces count (see header comment).
constexpr bool overlaps(const Aabb& a, const Aabb& b) {
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y &&
           a.min.z <= b.max.z && a.max.z >= b.min.z;
}

// Slab method. Hits from inside report t_min = 0 semantics via the interval
// [t_out_min, t_out_max]; returns false when the whole intersection interval
// is behind the origin. Axis-parallel rays produce +/-inf slabs — IEEE
// semantics keep the min/max lattice exact (Williams et al. 2005).
inline bool ray_aabb(const Ray& ray, const Aabb& box, float& t_out_min, float& t_out_max) {
    float t_min = 0.0f;
    float t_max = std::numeric_limits<float>::infinity();
    for (int axis = 0; axis < 3; ++axis) {
        // Axis-parallel rays need inv = +/-inf carrying the sign of the
        // (possibly negative) zero — exactly what IEEE 1/±0 yields. Computing
        // it via copysign instead of a literal divide-by-zero is bit-identical
        // and sidesteps MSVC's backend C4723, which no statement-level pragma
        // can reach (it fires at the inlining site).
        const float d = ray.dir[axis];
        float inv;
        if (d == 0.0f) {
            inv = std::copysign(std::numeric_limits<float>::infinity(), d);
        } else {
            inv = 1.0f / d;
        }
        float t0 = (box.min[axis] - ray.origin[axis]) * inv;
        float t1 = (box.max[axis] - ray.origin[axis]) * inv;
        if (inv < 0.0f) {
            const float tmp = t0;
            t0 = t1;
            t1 = tmp;
        }
        t_min = t0 > t_min ? t0 : t_min;
        t_max = t1 < t_max ? t1 : t_max;
        if (t_max < t_min)
            return false;
    }
    t_out_min = t_min;
    t_out_max = t_max;
    return true;
}

// Stable quadratic form (RTCD 5.3.2). Requires |ray.dir| > 0; t is in units
// of |ray.dir|. Origin inside the sphere reports the exit point (t >= 0).
inline bool ray_sphere(const Ray& ray, const Sphere& sphere, float& t_out) {
    const Vec3 m = ray.origin - sphere.center;
    const float dd = dot(ray.dir, ray.dir);
    const float b = dot(m, ray.dir);
    const float c = dot(m, m) - sphere.radius * sphere.radius;
    if (c > 0.0f && b > 0.0f)
        return false; // outside and pointing away
    const float disc = b * b - dd * c;
    if (disc < 0.0f)
        return false;
    const float sq = std::sqrt(disc);
    float t = (-b - sq) / dd;
    if (t < 0.0f)
        t = (-b + sq) / dd; // origin inside: exit point
    if (t < 0.0f)
        return false;
    t_out = t;
    return true;
}

// Moller-Trumbore 1997 (Godot geometry_3d.h shape). No backface culling;
// u/v are the barycentric coordinates of v1/v2. Rejects near-parallel rays
// with kEps on the determinant and grazing hits with t <= kEps (Godot uses
// the same guard value).
inline bool
ray_triangle(const Ray& ray, Vec3 v0, Vec3 v1, Vec3 v2, float& t_out, float& u_out, float& v_out) {
    constexpr float kEps = 1e-5f;
    const Vec3 e1 = v1 - v0;
    const Vec3 e2 = v2 - v0;
    const Vec3 h = cross(ray.dir, e2);
    const float a = dot(e1, h);
    if (a > -kEps && a < kEps)
        return false; // parallel
    const float f = 1.0f / a;
    const Vec3 s = ray.origin - v0;
    const float u = f * dot(s, h);
    if (u < 0.0f || u > 1.0f)
        return false;
    const Vec3 q = cross(s, e1);
    const float v = f * dot(ray.dir, q);
    if (v < 0.0f || u + v > 1.0f)
        return false;
    const float t = f * dot(e2, q);
    if (t <= kEps)
        return false; // behind or grazing the origin
    t_out = t;
    u_out = u;
    v_out = v;
    return true;
}

} // namespace midday::math
