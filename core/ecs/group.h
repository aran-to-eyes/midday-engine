// core/ecs/group.h — DESIGN of opt-in owning packed groups (EnTT's hybrid:
// sparse-set base + physically packed layouts for hot queries). This header
// is the committed design contract; the IMPLEMENTATION lands at m2-jobs,
// where the job system's measurement harness exists to prove the win (plan
// graft Meridian D3 — "BUILT and measured here"). Instantiating OwningGroup
// before then is a compile error, not a silently-working stub.
//
// WHAT A GROUP IS
//   An OwningGroup<A, B, ...> takes OWNERSHIP of the pools for A, B, ...
//   and maintains this physical invariant: entities possessing ALL owned
//   components occupy dense positions [0, group_size) in EVERY owned pool,
//   in the SAME order. Iteration then degenerates to a parallel walk of
//   packed value arrays — zero sparse lookups, zero membership branches —
//   plus the usual active-word scan over the group prefix.
//
// OWNERSHIP RULES
//   * A pool may be owned by AT MOST ONE group; requesting overlapping
//     ownership is a registration-time loud abort (same policy as duplicate
//     reflection names). Groups therefore partition the hot component
//     combinations — choosing them is a profiling decision, made at
//     m2-jobs with measurements attached.
//   * Groups are created at boot (before entities exist, or paying a one-
//     time sort of existing rows); creation mid-iteration is refused.
//   * Non-owned "observed" filter types (EnTT's partial groups) are OUT of
//     this design until a measured consumer demands them.
//
// INVARIANTS THE IMPLEMENTATION MUST KEEP
//   * Prefix maintenance is O(1) per structural op: when an entity gains
//     its last owned component, each owned pool swaps that entity's row to
//     position group_size, then group_size++; losing one (despawn) swaps to
//     group_size-1, then group_size--. Dense order inside the prefix is
//     therefore admission order — a pure function of the operation
//     sequence, deterministic across runs (spec section 4.3).
//   * ACTIVE FLAGS STAY ORTHOGONAL: toggling active/inactive remains a bit
//     write and NEVER moves rows in or out of the prefix (a toggle is not a
//     structural op — spec section 4.1's persistence rule). Iteration
//     word-scans active bits inside [0, group_size), exactly like views.
//   * Swap-and-pop on non-grouped rows must not disturb the prefix; all
//     row movement funnels through the SparseSet so sparse/dense/active
//     stay coherent.
//
// WHEN GROUPS BEAT VIEWS
//   A view pays one paged sparse lookup + one active-bit test per non-driver
//   pool per candidate row (core/ecs/view.h cost model). A group pays zero —
//   but every structural op touching an owned pool pays the O(#owned pools)
//   swap bookkeeping. Groups win when a multi-component query runs every
//   tick over large, heavily-overlapping pools whose membership churns
//   rarely (the transform/velocity/render kind of query); views win for
//   cold or churn-heavy combinations. m2-jobs decides with numbers, not
//   vibes.

#pragma once

namespace midday::ecs {

namespace detail {

template <typename...> inline constexpr bool kAlwaysFalse = false;

} // namespace detail

template <typename... Owned> class OwningGroup {
    static_assert(detail::kAlwaysFalse<Owned...>,
                  "OwningGroup is design-only at m0-ecs-core; the implementation lands at "
                  "m2-jobs with measurements (plan graft Meridian D3). Use World::view<> "
                  "until then.");
};

} // namespace midday::ecs
