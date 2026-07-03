// core/hierarchy/tree_order.cpp — the deterministic depth-first total order
// (Aurora D-4: THE default ordering for watchers, sequences, hooks, test
// queries).
//
// Representation & cost model: each Node caches a dense order index; one
// O(adopted) iterative DFS rebuilds every index after a topology-changing
// batch (adopt / structural flush / despawn), triggered lazily by the first
// order query. Comparisons are then O(1) loads — never O(n) scans — and a
// burst of topology changes costs ONE rebuild, not one per change. Memory:
// 4 bytes per node. Rebuild-order values are a pure function of the
// operation script (root list = adoption order, sibling lists = attach
// order), so indices are deterministic across runs and platforms.

#include "core/ecs/entity.h"
#include "core/hierarchy/hierarchy.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace midday::hierarchy {

void Hierarchy::rebuild_order() {
    std::uint32_t counter = 0;
    std::vector<ecs::EntityRef> stack;
    for (ecs::EntityRef root = first_root_; !root.is_null(); root = node_of(root)->next_sibling) {
        stack.push_back(root);
        while (!stack.empty()) {
            const ecs::EntityRef entity = stack.back();
            stack.pop_back();
            Node& node = *node_of(entity);
            node.order = counter++;
            // Push children reversed so the FIRST child pops first: parent
            // before children, siblings in attach order.
            for (ecs::EntityRef child = node.last_child; !child.is_null();
                 child = node_of(child)->prev_sibling)
                stack.push_back(child);
        }
    }
    order_dirty_ = false;
}

std::optional<std::uint32_t> Hierarchy::order_index(ecs::EntityRef entity) {
    if (node_of(entity) == nullptr)
        return std::nullopt;
    if (order_dirty_)
        rebuild_order();
    return node_of(entity)->order;
}

int Hierarchy::compare(ecs::EntityRef a, ecs::EntityRef b) {
    if (a == b)
        return 0;
    const bool a_adopted = contains(a);
    const bool b_adopted = contains(b);
    if (a_adopted && b_adopted) {
        if (order_dirty_)
            rebuild_order();
        const std::uint32_t oa = node_of(a)->order;
        const std::uint32_t ob = node_of(b)->order;
        return oa < ob ? -1 : 1; // distinct adopted entities: distinct indices
    }
    if (a_adopted != b_adopted)
        return a_adopted ? -1 : 1; // adopted entities precede unadopted ones
    // Both unadopted: slot index, then generation (total, deterministic
    // under LIFO reuse — provisional until the loader adopts everything).
    if (a.index != b.index)
        return a.index < b.index ? -1 : 1;
    return a.generation < b.generation ? -1 : 1;
}

} // namespace midday::hierarchy
