// core/physics/jolt_support.h — INTERNAL Jolt plumbing for the physics
// server TU(s): process-wide boot, the M0 collision-layer scheme, math
// conversions, and the contact collector. Never installed above core/
// physics; Jolt types exist only behind this seam (physics_server.h stays
// Jolt-free).

#pragma once

#include "core/base/json.h"
#include "core/math/quat.h"
#include "core/math/vec.h"

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <utility>
#include <vector>

// Jolt mandates <Jolt/Jolt.h> before any other Jolt header.
// clang-format off
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/RegisterTypes.h>
// clang-format on

namespace midday::physics::jolt {

// ---- process-wide boot (once; allocator, factory, type registry) ----------

inline void trace_noop(const char* /*fmt*/, ...) {} // no stdout noise, ever

inline void ensure_runtime() {
    static const bool booted = [] {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = trace_noop;
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        return true;
    }();
    (void)booted;
}

// ---- collision layers (M0: static vs moving; named project layers are m4) --

constexpr JPH::ObjectLayer kLayerStatic = 0;
constexpr JPH::ObjectLayer kLayerMoving = 1;

constexpr JPH::BroadPhaseLayer kBpStatic{0};
constexpr JPH::BroadPhaseLayer kBpMoving{1};
constexpr JPH::uint kBpLayerCount = 2;

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    [[nodiscard]] JPH::uint GetNumBroadPhaseLayers() const override { return kBpLayerCount; }

    [[nodiscard]] JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return layer == kLayerStatic ? kBpStatic : kBpMoving;
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    [[nodiscard]] const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override {
        return "";
    }
#endif
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer layer,
                                     JPH::BroadPhaseLayer bp_layer) const override {
        // Static collides only with moving; moving collides with everything.
        return layer == kLayerMoving || bp_layer == kBpMoving;
    }
};

class ObjectLayerPairs final : public JPH::ObjectLayerPairFilter {
public:
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return a == kLayerMoving || b == kLayerMoving;
    }
};

// ---- conversions ------------------------------------------------------------

[[nodiscard]] inline JPH::Vec3 to_jolt(math::Vec3 v) {
    return {v.x, v.y, v.z};
}

[[nodiscard]] inline math::Vec3 from_jolt(JPH::Vec3Arg v) {
    return {v.GetX(), v.GetY(), v.GetZ()};
}

[[nodiscard]] inline JPH::Quat to_jolt(math::Quat q) {
    return {q.x, q.y, q.z, q.w};
}

[[nodiscard]] inline math::Quat from_jolt(JPH::QuatArg q) {
    return {q.GetX(), q.GetY(), q.GetZ(), q.GetW()};
}

[[nodiscard]] inline base::Json vec3_json(math::Vec3 v) {
    base::Json array = base::Json::array();
    array.push(base::Json(static_cast<double>(v.x)));
    array.push(base::Json(static_cast<double>(v.y)));
    array.push(base::Json(static_cast<double>(v.z)));
    return array;
}

// ---- contact collection (during the step) ------------------------------------

// One collected pair, canonicalized to a < b by body-id bits so the phase-6
// sort key is the pair id itself (MILESTONE_0 item 17).
struct PendingContact {
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    math::Vec3 position;   // first manifold point, world space (began only)
    math::Vec3 normal_a_b; // world-space normal from a toward b (began only)

    [[nodiscard]] std::uint64_t pair_id() const {
        return (static_cast<std::uint64_t>(a) << 32U) | b;
    }
};

// Collects began/ended pairs into buffers; dispatch happens AFTER the step.
// M0 runs Jolt's single-threaded job system, so callbacks arrive on the
// stepping thread — no locking (m4 adds it with the thread-pool config).
class ContactCollector final : public JPH::ContactListener {
public:
    std::vector<PendingContact> began;
    std::vector<PendingContact> ended;

    void OnContactAdded(const JPH::Body& body1,
                        const JPH::Body& body2,
                        const JPH::ContactManifold& manifold,
                        JPH::ContactSettings& /*settings*/) override {
        PendingContact contact;
        contact.a = body1.GetID().GetIndexAndSequenceNumber();
        contact.b = body2.GetID().GetIndexAndSequenceNumber();
        contact.position = from_jolt(manifold.GetWorldSpaceContactPointOn1(0));
        // Jolt's manifold normal moves body 2 out of collision: a -> b when
        // body1 is a. Canonicalize to a < b, flipping the normal with it.
        contact.normal_a_b = from_jolt(manifold.mWorldSpaceNormal);
        if (contact.a > contact.b) {
            std::swap(contact.a, contact.b);
            contact.normal_a_b = -contact.normal_a_b;
        }
        began.push_back(contact);
    }

    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        PendingContact contact;
        contact.a = pair.GetBody1ID().GetIndexAndSequenceNumber();
        contact.b = pair.GetBody2ID().GetIndexAndSequenceNumber();
        if (contact.a > contact.b)
            std::swap(contact.a, contact.b);
        ended.push_back(contact);
    }
};

// Sort by pair id (stable: collection order breaks ties), then drop
// duplicate pairs — compound shapes may report one body pair per sub-shape
// pair; the bus event vocabulary speaks in body pairs.
inline void sort_unique(std::vector<PendingContact>& contacts) {
    std::ranges::stable_sort(contacts, {}, &PendingContact::pair_id);
    const auto duplicates =
        std::ranges::unique(contacts, [](const PendingContact& x, const PendingContact& y) {
            return x.pair_id() == y.pair_id();
        });
    contacts.erase(duplicates.begin(), duplicates.end());
}

} // namespace midday::physics::jolt
