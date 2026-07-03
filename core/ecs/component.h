// core/ecs/component.h — runtime component type identity.
//
// component_type_id<T>() hands each component TYPE a small dense integer,
// assigned on first touch (World pool slots index by it). It is a RUNTIME-
// ONLY key: never serialized, never journaled, never allowed to leak into
// any output — the persistent identity of a component class is its
// registered reflection Name (core/reflect/registry.h), whose id is a pure
// content hash (D-BUILD-011). Deterministic walks (despawn, enumeration)
// therefore always use per-World REGISTRATION order, never type-id order.

#pragma once

#include <cstdint>
#include <type_traits>

namespace midday::ecs {

namespace detail {

// Monotonic process counter (defined in world.cpp).
std::uint32_t next_component_type_id();

} // namespace detail

template <typename T> [[nodiscard]] std::uint32_t component_type_id() {
    static_assert(std::is_same_v<T, std::remove_cvref_t<T>>,
                  "component types are plain class types — no cv/ref qualifiers");
    static const std::uint32_t id = detail::next_component_type_id();
    return id;
}

} // namespace midday::ecs
