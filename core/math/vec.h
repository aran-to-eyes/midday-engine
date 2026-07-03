// core/math/vec.h — float32 vector types: Vec2, Vec3, Vec4.
//
// Scalar policy (spec section 4.3, MILESTONE_0 "real_t single-precision default"):
// the sim scalar is float (32-bit IEEE 754). double appears only where a
// deliverable demands it (RNG distribution internals, arc-length accumulation)
// and is documented at the use site.
//
// Determinism class of every operation in this header: BIT-PORTABLE.
// Only +, -, *, / and sqrt are used — all IEEE-correctly-rounded on every
// supported toolchain under the deterministic-FP policy (see core/math/README.md).
//
// Types are aggregates: brace-init (`Vec3{1, 2, 3}`), value-init to zero.
// operator* on two vectors is the component-wise (Hadamard) product.
// normalized() of a zero vector returns the zero vector (documented, tested) —
// callers that need a guaranteed unit vector use normalized_or().

#pragma once

#include <cmath>
#include <numbers>

namespace midday::math {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;

    constexpr float& operator[](int i) { return i == 0 ? x : y; }

    constexpr float operator[](int i) const { return i == 0 ? x : y; }

    friend constexpr Vec2 operator+(Vec2 a, Vec2 b) { return {a.x + b.x, a.y + b.y}; }

    friend constexpr Vec2 operator-(Vec2 a, Vec2 b) { return {a.x - b.x, a.y - b.y}; }

    friend constexpr Vec2 operator-(Vec2 a) { return {-a.x, -a.y}; }

    friend constexpr Vec2 operator*(Vec2 a, Vec2 b) { return {a.x * b.x, a.y * b.y}; }

    friend constexpr Vec2 operator*(Vec2 a, float s) { return {a.x * s, a.y * s}; }

    friend constexpr Vec2 operator*(float s, Vec2 a) { return a * s; }

    friend constexpr Vec2 operator/(Vec2 a, float s) { return {a.x / s, a.y / s}; }

    friend constexpr bool operator==(Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; }

    [[nodiscard]] constexpr float length_squared() const { return x * x + y * y; }

    [[nodiscard]] float length() const { return std::sqrt(length_squared()); }

    // Zero vector in -> zero vector out (no NaN escape hatch into sim state).
    [[nodiscard]] Vec2 normalized() const {
        const float len = length();
        return len > 0.0f ? *this / len : Vec2{};
    }
};

constexpr float dot(Vec2 a, Vec2 b) {
    return a.x * b.x + a.y * b.y;
}

// 2D "cross": the z component of the 3D cross of the embedded vectors.
constexpr float cross(Vec2 a, Vec2 b) {
    return a.x * b.y - a.y * b.x;
}

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;

    constexpr float& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }

    constexpr float operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }

    friend constexpr Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }

    friend constexpr Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }

    friend constexpr Vec3 operator-(Vec3 a) { return {-a.x, -a.y, -a.z}; }

    friend constexpr Vec3 operator*(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }

    friend constexpr Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }

    friend constexpr Vec3 operator*(float s, Vec3 a) { return a * s; }

    friend constexpr Vec3 operator/(Vec3 a, float s) { return {a.x / s, a.y / s, a.z / s}; }

    friend constexpr bool operator==(Vec3 a, Vec3 b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }

    [[nodiscard]] constexpr float length_squared() const { return x * x + y * y + z * z; }

    [[nodiscard]] float length() const { return std::sqrt(length_squared()); }

    [[nodiscard]] Vec3 normalized() const {
        const float len = length();
        return len > 0.0f ? *this / len : Vec3{};
    }

    // For directions that MUST be unit: degenerate input falls back to `fallback`.
    [[nodiscard]] Vec3 normalized_or(Vec3 fallback) const {
        const float len = length();
        return len > 0.0f ? *this / len : fallback;
    }
};

constexpr float dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

constexpr Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

struct Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;

    constexpr float& operator[](int i) { return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w)); }

    constexpr float operator[](int i) const { return i == 0 ? x : (i == 1 ? y : (i == 2 ? z : w)); }

    [[nodiscard]] constexpr Vec3 xyz() const { return {x, y, z}; }

    friend constexpr Vec4 operator+(Vec4 a, Vec4 b) {
        return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
    }

    friend constexpr Vec4 operator-(Vec4 a, Vec4 b) {
        return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
    }

    friend constexpr Vec4 operator*(Vec4 a, float s) {
        return {a.x * s, a.y * s, a.z * s, a.w * s};
    }

    friend constexpr Vec4 operator*(float s, Vec4 a) { return a * s; }

    friend constexpr bool operator==(Vec4 a, Vec4 b) {
        return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
    }

    [[nodiscard]] constexpr float length_squared() const { return x * x + y * y + z * z + w * w; }

    [[nodiscard]] float length() const { return std::sqrt(length_squared()); }
};

constexpr float dot(Vec4 a, Vec4 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
}

// Shared scalar helpers (bit-portable: arithmetic only).
constexpr float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

constexpr Vec2 lerp(Vec2 a, Vec2 b, float t) {
    return a + (b - a) * t;
}

constexpr Vec3 lerp(Vec3 a, Vec3 b, float t) {
    return a + (b - a) * t;
}

constexpr float clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

constexpr float saturate(float v) {
    return clamp(v, 0.0f, 1.0f);
}

constexpr Vec3 vec_min(Vec3 a, Vec3 b) {
    return {a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.z < b.z ? a.z : b.z};
}

constexpr Vec3 vec_max(Vec3 a, Vec3 b) {
    return {a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y, a.z > b.z ? a.z : b.z};
}

constexpr Vec3 vec_abs(Vec3 a) {
    return {a.x < 0.0f ? -a.x : a.x, a.y < 0.0f ? -a.y : a.y, a.z < 0.0f ? -a.z : a.z};
}

inline constexpr float kPi = std::numbers::pi_v<float>;
inline constexpr float kTau = 2.0f * std::numbers::pi_v<float>;

// Comparison helper for tests and geometric epsilon guards; NOT a substitute
// for exact bit equality where the determinism contract demands it.
constexpr bool almost_equal(float a, float b, float eps = 1e-5f) {
    const float d = a - b;
    return (d < 0.0f ? -d : d) <= eps;
}

} // namespace midday::math
