# core/ecs

Sparse-set ECS substrate (m0-ecs-core) — the storage layer under the statechart entity
model (spec §4.1). Sparse sets over archetypes because state changes toggle component
activity constantly, and here a toggle is one bit write instead of a table move
(ENGINE_ARCHITECTURE_RESEARCH.md §3 — the load-bearing input to this decision).

## Shape

- `entity.h` — generational `EntityRef` (32-bit index + 32-bit generation) + slot table.
  LIFO slot reuse; stale handles are structured Errors (`ecs.stale_handle`), never UB.
- `sparse_set.h` — paged sparse (4096-entry pages: shift/mask addressing, 16 KiB worst-case
  waste per touched region) → packed dense rows + one active bit per row in 64-bit words.
- `pool.h` — `Pool<T>`: dense value array parallel to the sparse set. Swap-and-pop on
  despawn only; components persist for the entity lifetime, activity toggles (spec §4.1).
- `view.h` — non-owning views. Default = ACTIVE rows of every queried component;
  `include_inactive()` is the explicit opt-in. Driver pool (smallest) is word-scanned:
  one 64-bit load per 64 rows + `countr_zero` per visit; other pools cost one paged
  lookup + bit test per candidate. No virtuals, no `std::function` on the path.
- `group.h` — owning packed groups: DESIGN ONLY (full contract in the header);
  implementation + measurements land at m2-jobs (plan graft Meridian D3).
- `structural_queue.h` / `world.h` — deferred spawn/despawn/reparent commands, applied at
  one flush point in queue order; direct structural mutation during iteration is a
  structured error (`ecs.structural_during_iteration`). `queue_spawn` reserves the handle
  immediately (pending state), live at flush. Reparent is the slot m0-scene-hierarchy
  fills via `set_reparent_handler` — tree topology lives in core/hierarchy, not here.
- Registration bridge: `World::register_component<T>(ClassDesc)` — one call registers the
  class in core/reflect (engine_api.json sees every component) and creates the pool.

## Determinism

Every observable order is a pure function of the operation sequence: LIFO slot reuse,
swap-and-pop dense order, per-World registration order for all-pool walks, queue order
for the flush. Toggle history NEVER reorders iteration (bits, not moves) — pinned by
XXH3 visit-order hashes across two independent runs in `ecs.determinism`.
