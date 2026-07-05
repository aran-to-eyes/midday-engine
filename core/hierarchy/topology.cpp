// core/hierarchy/topology.cpp — tree link surgery and the two ECS sockets
// (reparent handler, despawn observer). Contract in hierarchy.h.

#include "core/base/error.h"
#include "core/base/json.h"
#include "core/base/name.h"
#include "core/ecs/entity.h"
#include "core/hierarchy/hierarchy.h"
#include "core/reflect/registry.h"

#include <cassert>
#include <optional>
#include <string>
#include <vector>

namespace midday::hierarchy {

namespace detail {

namespace {

base::Error entity_error(std::string code, std::string message, ecs::EntityRef ref) {
    base::Error error;
    error.code = std::move(code);
    error.message = std::move(message);
    error.details.set("index", base::Json(ref.index));
    error.details.set("generation", base::Json(ref.generation));
    return error;
}

} // namespace

base::Error not_adopted_error(ecs::EntityRef ref) {
    return entity_error("hierarchy.not_adopted", "entity is not in the hierarchy", ref);
}

base::Error already_adopted_error(ecs::EntityRef ref) {
    return entity_error("hierarchy.already_adopted", "entity is already in the hierarchy", ref);
}

base::Error cycle_error(ecs::EntityRef child, ecs::EntityRef parent) {
    base::Error error = entity_error(
        "hierarchy.cycle", "attaching an entity under itself would create a cycle", child);
    error.details.set("parent_index", base::Json(parent.index));
    return error;
}

base::Error not_deactivated_error(ecs::EntityRef ref) {
    return entity_error(
        "hierarchy.not_deactivated", "entity is not an active deactivation scope root", ref);
}

base::Error already_deactivated_error(ecs::EntityRef ref) {
    return entity_error(
        "hierarchy.already_deactivated", "entity is already a deactivation scope root", ref);
}

} // namespace detail

namespace {

reflect::ClassDesc component_class(const char* name, const char* doc) {
    reflect::ClassDesc cls;
    cls.name = base::Name(name);
    cls.doc = doc;
    return cls;
}

} // namespace

Hierarchy::Hierarchy(ecs::World& world) : world_(&world) {
    world_->register_component<Node>(
        component_class("HierarchyNode",
                        "Entity tree links: parent/ordered children, owner boundary, "
                        "dormancy state (written only by hierarchy::Hierarchy)."));
    world_->register_component<LocalTransform>(
        component_class("LocalTransform", "Authored local TRS transform (core/math xform)."));
    world_->register_component<WorldTransform>(component_class(
        "WorldTransform", "Propagated local-to-world matrix (Hierarchy::propagate)."));
    world_->set_reparent_handler([this](ecs::EntityRef child, ecs::EntityRef new_parent) {
        apply_reparent(child, new_parent);
    });
    world_->set_despawn_observer([this](ecs::EntityRef entity) { on_despawn(entity); });
}

Hierarchy::~Hierarchy() {
    world_->set_reparent_handler({});
    world_->set_despawn_observer({});
}

Node* Hierarchy::node_of(ecs::EntityRef entity) {
    return world_->try_get<Node>(entity);
}

const Node* Hierarchy::node_of(ecs::EntityRef entity) const {
    return world_->try_get<Node>(entity);
}

// ---- adoption --------------------------------------------------------------

std::optional<base::Error> Hierarchy::adopt(ecs::EntityRef entity) {
    if (contains(entity))
        return detail::already_adopted_error(entity);
    return adopt_now(entity);
}

std::optional<base::Error> Hierarchy::adopt_now(ecs::EntityRef entity) {
    // The first emplace carries every refusal (iteration lock, stale or
    // pending handle); the trailing two cannot fail after it succeeded.
    if (auto error = world_->emplace<Node>(entity, Node{}))
        return error;
    [[maybe_unused]] auto local_error = world_->emplace<LocalTransform>(entity, LocalTransform{});
    assert(!local_error);
    [[maybe_unused]] auto world_error = world_->emplace<WorldTransform>(entity, WorldTransform{});
    assert(!world_error);

    Node& node = *node_of(entity); // fetched after the emplaces (pool growth moves rows)
    link_last(entity, node, ecs::EntityRef{});
    node.transform_dirty = true;
    any_transform_dirty_ = true;
    ++adopted_count_;
    order_dirty_ = true;
    return std::nullopt;
}

bool Hierarchy::contains(ecs::EntityRef entity) const {
    return world_->has<Node>(entity);
}

// ---- topology reads ---------------------------------------------------------

ecs::EntityRef Hierarchy::parent_of(ecs::EntityRef entity) const {
    const Node* node = node_of(entity);
    return node == nullptr ? ecs::EntityRef{} : node->parent;
}

ecs::EntityRef Hierarchy::first_child_of(ecs::EntityRef entity) const {
    const Node* node = node_of(entity);
    return node == nullptr ? ecs::EntityRef{} : node->first_child;
}

ecs::EntityRef Hierarchy::next_sibling_of(ecs::EntityRef entity) const {
    const Node* node = node_of(entity);
    return node == nullptr ? ecs::EntityRef{} : node->next_sibling;
}

std::uint32_t Hierarchy::child_count(ecs::EntityRef entity) const {
    std::uint32_t count = 0;
    for (ecs::EntityRef child = first_child_of(entity); !child.is_null();
         child = node_of(child)->next_sibling)
        ++count;
    return count;
}

// ---- owner boundary ----------------------------------------------------------

std::optional<base::Error> Hierarchy::set_owner(ecs::EntityRef entity, bool owner) {
    Node* node = node_of(entity);
    if (node == nullptr)
        return detail::not_adopted_error(entity);
    node->owner = owner;
    return std::nullopt;
}

bool Hierarchy::is_owner(ecs::EntityRef entity) const {
    const Node* node = node_of(entity);
    return node != nullptr && node->owner;
}

ecs::EntityRef Hierarchy::owner_of(ecs::EntityRef entity) const {
    for (ecs::EntityRef at = entity; !at.is_null();) {
        const Node* node = node_of(at);
        if (node == nullptr)
            return ecs::EntityRef{};
        if (node->owner)
            return at;
        at = node->parent;
    }
    return ecs::EntityRef{};
}

// ---- structural mutation: queue sugar ----------------------------------------

std::optional<base::Error> Hierarchy::queue_attach(ecs::EntityRef child,
                                                   ecs::EntityRef new_parent) {
    // Only the always-degenerate self-parent case is refusable at queue
    // time; deeper cycles depend on topology at APPLY time (earlier queued
    // commands may move subtrees) and are counted no-ops there.
    if (!new_parent.is_null() && new_parent == child)
        return detail::cycle_error(child, new_parent);
    return world_->queue_reparent(child, new_parent);
}

std::optional<base::Error> Hierarchy::queue_detach(ecs::EntityRef child) {
    return world_->queue_reparent(child, ecs::EntityRef{});
}

// ---- link surgery (private; flush/adopt/despawn paths only) -------------------

void Hierarchy::link_last(ecs::EntityRef entity, Node& node, ecs::EntityRef parent) {
    Node* parent_node = parent.is_null() ? nullptr : node_of(parent);
    const ecs::EntityRef last = parent_node == nullptr ? last_root_ : parent_node->last_child;
    node.parent = parent;
    node.prev_sibling = last;
    node.next_sibling = ecs::EntityRef{};
    if (!last.is_null()) {
        node_of(last)->next_sibling = entity;
    } else if (parent_node == nullptr) {
        first_root_ = entity;
    } else {
        parent_node->first_child = entity;
    }
    if (parent_node == nullptr) {
        last_root_ = entity;
    } else {
        parent_node->last_child = entity;
    }
}

void Hierarchy::unlink(Node& node) {
    Node* parent_node = node.parent.is_null() ? nullptr : node_of(node.parent);
    if (!node.prev_sibling.is_null()) {
        node_of(node.prev_sibling)->next_sibling = node.next_sibling;
    } else if (parent_node == nullptr) {
        first_root_ = node.next_sibling;
    } else {
        parent_node->first_child = node.next_sibling;
    }
    if (!node.next_sibling.is_null()) {
        node_of(node.next_sibling)->prev_sibling = node.prev_sibling;
    } else if (parent_node == nullptr) {
        last_root_ = node.prev_sibling;
    } else {
        parent_node->last_child = node.prev_sibling;
    }
    node.parent = ecs::EntityRef{};
    node.prev_sibling = ecs::EntityRef{};
    node.next_sibling = ecs::EntityRef{};
}

bool Hierarchy::subtree_contains(ecs::EntityRef root, ecs::EntityRef entity) const {
    for (ecs::EntityRef at = entity; !at.is_null(); at = node_of(at)->parent) {
        if (at == root)
            return true;
    }
    return false;
}

void Hierarchy::collect_subtree(ecs::EntityRef root, std::vector<ecs::EntityRef>& out) const {
    // Iterative pre-order DFS, children in attach order (push reversed).
    std::vector<ecs::EntityRef> stack;
    stack.push_back(root);
    while (!stack.empty()) {
        const ecs::EntityRef entity = stack.back();
        stack.pop_back();
        out.push_back(entity);
        const Node& node = *node_of(entity);
        for (ecs::EntityRef child = node.last_child; !child.is_null();
             child = node_of(child)->prev_sibling)
            stack.push_back(child);
    }
}

// ---- the reparent socket (flush time, queue order) ----------------------------

void Hierarchy::apply_reparent(ecs::EntityRef child, ecs::EntityRef new_parent) {
    // Adopt strangers BEFORE taking Node pointers: emplace can move the pool.
    if (!contains(child)) {
        [[maybe_unused]] auto error = adopt_now(child);
        assert(!error); // flush re-validated liveness; flushes never iterate
        ++stats_.auto_adopted;
    }
    if (!new_parent.is_null() && !contains(new_parent)) {
        [[maybe_unused]] auto error = adopt_now(new_parent);
        assert(!error);
        ++stats_.auto_adopted;
    }

    if (!new_parent.is_null() && subtree_contains(child, new_parent)) {
        ++stats_.cycles_refused; // deterministic no-op, see hierarchy.h
        return;
    }

    Node& child_node = *node_of(child);
    const std::int64_t old_cover =
        child_node.parent.is_null()
            ? 0
            : static_cast<std::int64_t>(node_of(child_node.parent)->dormant_depth);
    const std::int64_t new_cover =
        new_parent.is_null() ? 0 : static_cast<std::int64_t>(node_of(new_parent)->dormant_depth);

    unlink(child_node);
    link_last(child, child_node, new_parent);
    if (new_cover != old_cover)
        apply_dormancy_delta(child, new_cover - old_cover);
    mark_transform_dirty(child_node);
    order_dirty_ = true;
    ++stats_.applied;
}

// ---- the despawn socket (cascade + unlink; rows still intact) ------------------

void Hierarchy::on_despawn(ecs::EntityRef entity) {
    if (!contains(entity))
        return;
    // Children die with their subtree root, depth-first. Refetch the node
    // every round: nested despawns swap-and-pop the Node pool, and each
    // child unlinks itself from our list via its own on_despawn.
    while (true) {
        const ecs::EntityRef child = node_of(entity)->first_child;
        if (child.is_null())
            break;
        [[maybe_unused]] auto error = world_->despawn(child);
        assert(!error); // alive by tree invariant; despawn paths never iterate
    }
    unlink(*node_of(entity));
    --adopted_count_;
    order_dirty_ = true;
}

} // namespace midday::hierarchy
