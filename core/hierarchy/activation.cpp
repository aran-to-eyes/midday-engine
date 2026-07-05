// core/hierarchy/activation.cpp — subtree activation: the states-as-nodes
// substrate (spec section 4.1: state active means subtree live; inactive
// means dormant but not destroyed).
//
// Mechanism: pure ACTIVE-BIT toggles through the ECS — zero memory
// movement, rows persist, pointer stability pinned by hierarchy.activation
// tests. Every deactivation scope, and every reparent across a dormancy
// boundary, reduces to ONE primitive: apply_dormancy_delta(root, ±1..n),
// which adjusts each subtree entity's covering-scope count and
//   * captures + clears its exact per-pool active pattern on 0 -> covered,
//   * restores that pattern bit-for-bit on covered -> 0.
// Nesting therefore falls out: outer scopes shadow inner ones (an inner
// activate under a covered ancestor just decrements the count — the entity
// stays dormant, its captured pattern untouched until the LAST scope lifts).
//
// Capture format (Node::saved_activity): pools walked in per-World
// REGISTRATION order (the deterministic all-pool order), interleaved
// [present, active] words per 64 pools. Restore touches only pools captured
// as PRESENT — a component emplaced while dormant keeps whatever bit it was
// given (document: pass active=false when building under a dormant root).

#include "core/ecs/entity.h"
#include "core/ecs/pool.h"
#include "core/ecs/sparse_set.h"
#include "core/hierarchy/hierarchy.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace midday::hierarchy {

std::optional<base::Error> Hierarchy::deactivate(ecs::EntityRef subtree_root) {
    Node* node = node_of(subtree_root);
    if (node == nullptr)
        return detail::not_adopted_error(subtree_root);
    if (node->deactivated)
        return detail::already_deactivated_error(subtree_root);
    node->deactivated = true;
    apply_dormancy_delta(subtree_root, 1);
    return std::nullopt;
}

std::optional<base::Error> Hierarchy::activate(ecs::EntityRef subtree_root) {
    Node* node = node_of(subtree_root);
    if (node == nullptr)
        return detail::not_adopted_error(subtree_root);
    if (!node->deactivated)
        return detail::not_deactivated_error(subtree_root);
    node->deactivated = false;
    apply_dormancy_delta(subtree_root, -1);
    return std::nullopt;
}

bool Hierarchy::is_dormant(ecs::EntityRef entity) const {
    const Node* node = node_of(entity);
    return node != nullptr && node->dormant_depth > 0;
}

bool Hierarchy::is_deactivated(ecs::EntityRef entity) const {
    const Node* node = node_of(entity);
    return node != nullptr && node->deactivated;
}

void Hierarchy::apply_dormancy_delta(ecs::EntityRef root, std::int64_t delta) {
    std::vector<ecs::EntityRef> entities;
    collect_subtree(root, entities);
    for (const ecs::EntityRef entity : entities) {
        Node& node = *node_of(entity);
        const std::uint32_t old_depth = node.dormant_depth;
        node.dormant_depth =
            static_cast<std::uint32_t>(static_cast<std::int64_t>(old_depth) + delta);
        if (old_depth == 0 && node.dormant_depth > 0) {
            capture_and_clear(entity, node);
        } else if (old_depth > 0 && node.dormant_depth == 0) {
            restore_activity(entity, node);
        }
    }
}

void Hierarchy::capture_and_clear(ecs::EntityRef entity, Node& node) {
    const auto pools = world_->pools_in_registration_order();
    const std::size_t groups = (pools.size() + 63u) / 64u;
    node.saved_activity.assign(groups * 2u, 0u);
    for (std::size_t i = 0; i < pools.size(); ++i) {
        const std::uint32_t pos = pools[i]->set().find(entity.index);
        if (pos == ecs::kNpos)
            continue;
        const std::uint64_t bit = std::uint64_t{1} << (i & 63u);
        node.saved_activity[(i >> 6u) * 2u] |= bit; // present
        if (pools[i]->set().is_active(pos))
            node.saved_activity[(i >> 6u) * 2u + 1u] |= bit; // active
        pools[i]->set_row_active(pos, false);                // one bit write, zero movement
    }
}

void Hierarchy::restore_activity(ecs::EntityRef entity, Node& node) {
    const auto pools = world_->pools_in_registration_order();
    // Pools registered after capture lie beyond the words: treated as
    // not-captured, exactly like rows emplaced while dormant.
    const std::size_t captured_pools = (node.saved_activity.size() / 2u) * 64u;
    for (std::size_t i = 0; i < pools.size() && i < captured_pools; ++i) {
        const std::uint64_t bit = std::uint64_t{1} << (i & 63u);
        if ((node.saved_activity[(i >> 6u) * 2u] & bit) == 0)
            continue; // not present at capture: leave the row's bit alone
        const std::uint32_t pos = pools[i]->set().find(entity.index);
        if (pos == ecs::kNpos)
            continue; // rows die only at despawn; guard stays for robustness
        pools[i]->set_row_active(pos, (node.saved_activity[(i >> 6u) * 2u + 1u] & bit) != 0);
    }
    node.saved_activity.clear();
}

} // namespace midday::hierarchy
