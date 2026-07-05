// core/hierarchy/hierarchy.h — the runtime entity tree (m0-scene-hierarchy):
// topology, deterministic depth-first order, transform propagation, and
// subtree activation — the substrate states-as-nodes stands on (spec
// section 4.1: state active means subtree live; Aurora D-4: deterministic
// tree order is THE default ordering for watchers, sequences, hooks, and
// test queries).
//
// Structural contract (impossible-by-construction, not by discipline):
//   * Reparent/attach/detach flow ONLY through the ECS structural queue —
//     Hierarchy installs the ReparentHandler at construction and exposes no
//     other topology mutator. queue_attach/queue_detach are sugar over
//     World::queue_reparent; surgery happens exclusively inside the flush
//     (World::flush_structural), which the ECS refuses mid-iteration.
//   * adopt() enters an entity as a new root. It is structural (component
//     emplacement) and therefore inherits the ECS iteration lock: calling
//     it mid-iteration is a structured error, never a mutation.
//   * Despawn cascades: despawning an adopted entity despawns its whole
//     subtree, depth-first, before the entity's own rows drop (the tree is
//     the ownership shape — machines/prefabs are subtrees). Wired through
//     World::set_despawn_observer.
//
// Reparent apply semantics (flush time, queue order):
//   * An unadopted child (including a spawn from the same queue) is
//     AUTO-ADOPTED first — queue_spawn + queue_reparent in one tick is the
//     canonical spawn-and-attach flow (D-BUILD-026 pending refs).
//   * Reparent ALWAYS appends to the target's child list — including a
//     reparent onto the current parent, which moves the child to the end
//     (the deterministic reorder primitive). Detach appends to the roots.
//   * A cycle (new parent inside the child's own subtree) is a counted,
//     deterministic no-op (reparent_stats().cycles_refused) — the handler
//     signature has no error channel, and refusing loudly at queue time is
//     impossible because earlier queued commands may legalize/illegalize
//     the move. queue_attach refuses only the always-degenerate
//     child == parent case up front.
//   * The child KEEPS its local transform (world changes); keep-world
//     reparenting is an editor-level operation built on top later.
//
// Tree order (tree_order.cpp): parent before children, siblings in attach
// order, roots in adoption order — a total order over live entities that is
// a pure function of the operation script. Representation: a cached order
// index per Node, rebuilt lazily by one O(n) DFS after a topology-changing
// batch (adopt/flush/despawn), so comparisons are O(1) loads and NEVER
// O(n) scans. Reparenting one subtree renumbers indices but preserves the
// relative order of every entity outside it (the DFS structure elsewhere is
// untouched).
//
// Activation (activation.cpp): deactivate(root) toggles the ACTIVE BITS of
// every component row in the subtree — zero memory movement, rows persist
// (spec 4.1) — after capturing each entity's exact per-pool active pattern.
// activate(root) restores that pattern bit-for-bit. Scopes nest: an entity
// is dormant while ANY ancestor-or-self scope covers it (outer deactivation
// shadows inner: activating an inner scope under a deactivated outer one
// keeps the subtree dormant and preserves its captured pattern until the
// outer scope lifts). Reparenting across dormancy boundaries applies the
// cover delta to the moved subtree — moving into a dormant region captures
// and clears, moving out restores.
//
// Determinism: every observable — sibling order, root order, order indices,
// propagation results, activation patterns — is a pure function of the
// operation sequence (spec 4.3), pinned by hierarchy.order dual-run tests.

#pragma once

#include "core/base/error.h"
#include "core/ecs/world.h"
#include "core/hierarchy/components.h"
#include "core/math/mat.h"
#include "core/math/xform.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace midday::hierarchy {

namespace detail {

// Structured-error constructors (topology.cpp).
[[nodiscard]] base::Error not_adopted_error(ecs::EntityRef ref);
[[nodiscard]] base::Error already_adopted_error(ecs::EntityRef ref);
[[nodiscard]] base::Error cycle_error(ecs::EntityRef child, ecs::EntityRef parent);
[[nodiscard]] base::Error not_deactivated_error(ecs::EntityRef ref);
[[nodiscard]] base::Error already_deactivated_error(ecs::EntityRef ref);

} // namespace detail

class Hierarchy {
public:
    // Cumulative reparent-application counters (monotonic; tests take deltas).
    struct ReparentStats {
        std::uint64_t applied = 0;
        std::uint64_t cycles_refused = 0; // deterministic no-ops, see header
        std::uint64_t auto_adopted = 0;
    };

    // Registers HierarchyNode/LocalTransform/WorldTransform with the World
    // (boot path — one Hierarchy per World, constructed once) and installs
    // the reparent handler + despawn observer. The Hierarchy must outlive
    // every flush; the destructor uninstalls both sockets.
    explicit Hierarchy(ecs::World& world);
    ~Hierarchy();

    Hierarchy(const Hierarchy&) = delete;
    Hierarchy& operator=(const Hierarchy&) = delete;
    Hierarchy(Hierarchy&&) = delete;
    Hierarchy& operator=(Hierarchy&&) = delete;

    // ---- adoption (structural: refused mid-iteration) ---------------------
    // Enters a live entity as the LAST root and gives it identity transforms.
    std::optional<base::Error> adopt(ecs::EntityRef entity);

    [[nodiscard]] bool contains(ecs::EntityRef entity) const;

    [[nodiscard]] std::uint32_t adopted_count() const { return adopted_count_; }

    // ---- topology reads (null ref when absent/unadopted) ------------------
    [[nodiscard]] ecs::EntityRef parent_of(ecs::EntityRef entity) const;
    [[nodiscard]] ecs::EntityRef first_child_of(ecs::EntityRef entity) const;
    [[nodiscard]] ecs::EntityRef next_sibling_of(ecs::EntityRef entity) const;

    [[nodiscard]] ecs::EntityRef first_root() const { return first_root_; }

    [[nodiscard]] std::uint32_t child_count(ecs::EntityRef entity) const; // O(children)

    // ---- owner boundary (prefab/machine subtree roots later) --------------
    std::optional<base::Error> set_owner(ecs::EntityRef entity, bool owner);
    [[nodiscard]] bool is_owner(ecs::EntityRef entity) const;
    // Nearest ancestor-or-self marked owner; null when none.
    [[nodiscard]] ecs::EntityRef owner_of(ecs::EntityRef entity) const;

    // ---- structural mutation: queue only (see header contract) ------------
    std::optional<base::Error> queue_attach(ecs::EntityRef child, ecs::EntityRef new_parent);
    std::optional<base::Error> queue_detach(ecs::EntityRef child);

    [[nodiscard]] const ReparentStats& reparent_stats() const { return stats_; }

    // ---- deterministic tree order (cost model in the header) --------------
    // Depth-first index of an adopted entity (lazy O(n) rebuild when stale,
    // then O(1)); nullopt for unadopted/dead refs.
    [[nodiscard]] std::optional<std::uint32_t> order_index(ecs::EntityRef entity);

    // Total order over live entities: adopted entities by tree order,
    // adopted before unadopted, unadopted among themselves by slot index
    // then generation (deterministic under LIFO reuse — provisional until
    // the loader adopts every scene entity). Returns -1/0/+1.
    [[nodiscard]] int compare(ecs::EntityRef a, ecs::EntityRef b);

    // strict-weak-order comparator form for sorts.
    [[nodiscard]] bool tree_less(ecs::EntityRef a, ecs::EntityRef b) { return compare(a, b) < 0; }

    // ---- transform hierarchy ----------------------------------------------
    // Value write + dirty marking — allowed mid-iteration (moves nothing).
    std::optional<base::Error> set_local(ecs::EntityRef entity, const math::Transform& local);
    [[nodiscard]] const math::Transform* local_of(ecs::EntityRef entity) const;
    [[nodiscard]] const math::Mat4* world_of(ecs::EntityRef entity) const;

    // Recomputes world matrices for dirty subtrees, parents before children,
    // dormant entities included (a waking subtree is never stale). THE
    // deterministic phase point — called explicitly for now; m0-tick-loop
    // wires it into the tick order. Cost: O(dirty subtrees + their ancestor
    // paths) traversal, matrix work only on recomputed nodes; O(1) when
    // nothing is dirty.
    void propagate();

    // ---- subtree activation (states-as-nodes substrate) --------------------
    // Bit toggles only — zero memory movement; allowed mid-iteration with
    // the same word-visibility caveat as ecs view toggles.
    std::optional<base::Error> deactivate(ecs::EntityRef subtree_root);
    std::optional<base::Error> activate(ecs::EntityRef subtree_root);

    // Covered by at least one deactivation scope (ancestor-or-self).
    [[nodiscard]] bool is_dormant(ecs::EntityRef entity) const;

private:
    [[nodiscard]] Node* node_of(ecs::EntityRef entity);
    [[nodiscard]] const Node* node_of(ecs::EntityRef entity) const;

    // topology.cpp — link surgery + sockets. Node&/Node* arguments are
    // fetched AFTER any emplace (pool growth moves rows).
    std::optional<base::Error> adopt_now(ecs::EntityRef entity);
    void link_last(ecs::EntityRef entity, Node& node, ecs::EntityRef parent);
    void unlink(Node& node);
    [[nodiscard]] bool subtree_contains(ecs::EntityRef root, ecs::EntityRef entity) const;
    void collect_subtree(ecs::EntityRef root, std::vector<ecs::EntityRef>& out) const;
    void apply_reparent(ecs::EntityRef child, ecs::EntityRef new_parent);
    void on_despawn(ecs::EntityRef entity);

    // tree_order.cpp
    void rebuild_order();

    // transforms.cpp
    void mark_transform_dirty(Node& node);

    // Reused DFS work list for propagate() (cleared per call — the clean-forest
    // path never touches it).
    struct PropagateItem {
        ecs::EntityRef entity;
        bool parent_changed = false;
    };

    std::vector<PropagateItem> propagate_stack_;

    // activation.cpp — the one primitive under deactivate/activate/reparent:
    // adds `delta` covering scopes to every entity in the subtree, capturing
    // on 0 -> dormant and restoring on dormant -> 0 transitions.
    void apply_dormancy_delta(ecs::EntityRef root, std::int64_t delta);
    void capture_and_clear(ecs::EntityRef entity, Node& node);
    void restore_activity(ecs::EntityRef entity, Node& node);

    ecs::World* world_;
    ecs::EntityRef first_root_;
    ecs::EntityRef last_root_;
    std::uint32_t adopted_count_ = 0;
    bool order_dirty_ = false;
    bool any_transform_dirty_ = false;
    ReparentStats stats_;
};

} // namespace midday::hierarchy
