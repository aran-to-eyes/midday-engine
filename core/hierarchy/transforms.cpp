// core/hierarchy/transforms.cpp — dirty-flagged world transform propagation.
//
// Composition is MATRIX-space (world = parent_world * local.to_mat4()):
// TRS does not close under non-uniform scale (core/math/xform.h), and the
// per-node product order is fixed, so results are a pure function of the
// local transforms and the tree — BIT-PORTABLE per the math FP classes.
//
// Dirty scheme (the classic two-bit form): set_local/reparent marks the
// node `transform_dirty` and walks up setting `transform_dirty_below` until
// it meets a node already marked (amortized O(1) per mark). propagate()
// descends only into marked-or-recomputed branches, so a clean forest costs
// one flag test and a small edit costs its subtree plus the ancestor path.

#include "core/ecs/entity.h"
#include "core/hierarchy/hierarchy.h"
#include "core/math/mat.h"
#include "core/math/xform.h"

#include <optional>
#include <vector>

namespace midday::hierarchy {

std::optional<base::Error> Hierarchy::set_local(ecs::EntityRef entity,
                                                const math::Transform& local) {
    Node* node = node_of(entity);
    if (node == nullptr)
        return detail::not_adopted_error(entity);
    world_->try_get<LocalTransform>(entity)->value = local;
    mark_transform_dirty(*node);
    return std::nullopt;
}

const math::Transform* Hierarchy::local_of(ecs::EntityRef entity) const {
    const LocalTransform* local = world_->try_get<LocalTransform>(entity);
    return local == nullptr ? nullptr : &local->value;
}

const math::Mat4* Hierarchy::world_of(ecs::EntityRef entity) const {
    const WorldTransform* world = world_->try_get<WorldTransform>(entity);
    return world == nullptr ? nullptr : &world->value;
}

void Hierarchy::mark_transform_dirty(Node& node) {
    node.transform_dirty = true;
    any_transform_dirty_ = true;
    for (ecs::EntityRef at = node.parent; !at.is_null();) {
        Node& ancestor = *node_of(at);
        if (ancestor.transform_dirty_below)
            break; // the path above is already marked
        ancestor.transform_dirty_below = true;
        at = ancestor.parent;
    }
}

void Hierarchy::propagate() {
    if (!any_transform_dirty_)
        return;

    std::vector<PropagateItem>& stack = propagate_stack_;
    stack.clear();
    // Roots pushed reversed so the first root is processed first — parents
    // always precede children on any DFS; traversal order is cosmetic and
    // results are order-independent, but we keep it canonical anyway.
    for (ecs::EntityRef root = last_root_; !root.is_null(); root = node_of(root)->prev_sibling)
        stack.push_back(PropagateItem{root, false});
    while (!stack.empty()) {
        const PropagateItem item = stack.back();
        stack.pop_back();
        Node& node = *node_of(item.entity);
        const bool recompute = item.parent_changed || node.transform_dirty;
        if (recompute) {
            const math::Mat4 parent_world =
                node.parent.is_null() ? math::Mat4::identity()
                                      : world_->try_get<WorldTransform>(node.parent)->value;
            world_->try_get<WorldTransform>(item.entity)->value =
                parent_world * world_->try_get<LocalTransform>(item.entity)->value.to_mat4();
            node.transform_dirty = false;
        }
        if (recompute || node.transform_dirty_below) {
            node.transform_dirty_below = false;
            for (ecs::EntityRef child = node.last_child; !child.is_null();
                 child = node_of(child)->prev_sibling)
                stack.push_back(PropagateItem{child, recompute});
        }
    }
    any_transform_dirty_ = false;
}

} // namespace midday::hierarchy
