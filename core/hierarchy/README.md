# core/hierarchy

Runtime entity tree (m0-scene-hierarchy) — topology, deterministic tree order,
transform propagation, subtree activation. The substrate states-as-nodes stands
on (spec §4.1: state active ⇒ subtree live; Aurora D-4: deterministic tree order
is THE default ordering for watchers, sequences, hooks, test queries).

## Shape

- `components.h` — the ECS-resident data: `Node` (parent/ordered children as
  intrusive sibling links; the root set is itself a sibling list), `LocalTransform`
  (authored TRS), `WorldTransform` (propagated matrix). Written only through
  `Hierarchy`; readable like any component.
- `hierarchy.h` — the facade + full contract. One `Hierarchy` per `World`,
  constructed at boot; it registers the three components and installs both ECS
  sockets (reparent handler, despawn observer).
- `topology.cpp` — adoption (new roots; rides the ECS emplace path, so the
  iteration lock refuses it mid-iteration by construction), link surgery, the
  flush-time reparent apply (auto-adopt, cycle refusal as counted no-op,
  always-append incl. move-to-end), cascade despawn (D-BUILD-028/029).
- `tree_order.cpp` — cached DFS order indices, rebuilt lazily by one O(n) pass
  per topology-change batch: comparisons are O(1) loads, never scans. `compare`
  is a total order over live entities (D-BUILD-027).
- `transforms.cpp` — matrix-space composition (`world = parent_world *
  local.to_mat4()`, xform.h policy), two-bit dirty scheme (`dirty` +
  `dirty_below`), explicit `propagate()` as the deterministic phase point
  (m0-tick-loop wires it into the tick).
- `activation.cpp` — deactivation scopes: flag + covering-scope counts, exact
  per-pool activity capture/restore (bit toggles only, zero memory movement);
  outer scopes shadow inner; reparents across dormancy boundaries apply the
  cover delta (D-BUILD-030).

## Determinism

Sibling order = attach order, root order = adoption order, every reparent applies
at the structural flush in queue order — all observable state is a pure function
of the operation script, pinned by the hierarchy.order dual-run XXH3 digest.
