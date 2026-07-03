// core/hierarchy/components.h — the ECS-resident tree data (m0-scene-hierarchy).
//
// Topology lives IN component pools, not in a side object tree: every entity
// in the hierarchy carries a Node row (links), a LocalTransform row (TRS
// authored value), and a WorldTransform row (propagated matrix). The rows
// persist for the entity lifetime and toggle activity like every other
// component (spec section 4.1) — a dormant subtree keeps its topology bytes.
//
// Node fields are WRITTEN ONLY by hierarchy::Hierarchy (topology surgery is
// queue-flush-only; see hierarchy.h). Reading them directly is fine — they
// are ordinary component data — but mutating links by hand corrupts the tree
// exactly like writing a pool's internals would corrupt the ECS.

#pragma once

#include "core/ecs/entity.h"
#include "core/math/mat.h"
#include "core/math/xform.h"

#include <cstdint>
#include <vector>

namespace midday::hierarchy {

// Tree links + per-entity hierarchy state. Sibling order IS attach order:
// children append at last_child, and the root set is itself a sibling list
// (parent == null) ordered by adoption, so one link vocabulary covers both.
struct Node {
    ecs::EntityRef parent;       // null = root
    ecs::EntityRef first_child;  // attach order: oldest child
    ecs::EntityRef last_child;   // attach order: newest child (O(1) append)
    ecs::EntityRef prev_sibling; // among siblings, or among roots when parent is null
    ecs::EntityRef next_sibling;

    // Cached depth-first order index (tree_order.cpp). Valid only while the
    // Hierarchy's order cache is clean — always read through
    // Hierarchy::order_index / compare, which rebuild lazily.
    std::uint32_t order = 0;

    // Number of deactivation scopes covering this entity (its own flag plus
    // every flagged ancestor). Dormant iff > 0 (activation.cpp).
    std::uint32_t dormant_depth = 0;

    // Subtree ownership boundary (prefab/machine roots later): owner_of()
    // resolves the nearest ancestor-or-self with this flag.
    bool owner = false;

    // This entity is an explicit deactivation scope root (Hierarchy::
    // deactivate was called on it and not yet balanced by activate).
    bool deactivated = false;

    // Transform dirty flags (transforms.cpp): dirty = this entity's world
    // matrix needs recompute; dirty_below = some descendant does, so
    // propagate() must descend here even if this node is clean.
    bool transform_dirty = false;
    bool transform_dirty_below = false;

    // Captured activity pattern while dormant (empty otherwise): interleaved
    // 64-pool word groups [present, active, present, active, ...] over the
    // World's pools in registration order at capture time (activation.cpp).
    std::vector<std::uint64_t> saved_activity;
};

// Authored local TRS (composition semantics in core/math/xform.h). Write
// through Hierarchy::set_local so dirty flags stay truthful.
struct LocalTransform {
    math::Transform value;
};

// Propagated local-to-world matrix, valid after Hierarchy::propagate().
// Composition happens in matrix space (TRS does not close under non-uniform
// scale — xform.h): world = parent_world * local.to_mat4().
struct WorldTransform {
    math::Mat4 value;
};

} // namespace midday::hierarchy
