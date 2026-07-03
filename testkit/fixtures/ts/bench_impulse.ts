// Shared per-tick math for the script-bench fixtures: BOTH modes (batched
// typed-array views and naive per-field hooks) run exactly this arithmetic,
// so their final state hashes must match — the parity pin of bindings.bench.
// Pooled Vec3/Quat only: after warmup a tick allocates zero GC bytes.

import { Vec3, quatPool, vec3Pool } from "midday/math";

export const DT = 1 / 60;
export const DECAY = 0.125; // exactly representable: parity stays bit-exact

// A pooled steering impulse: a unit quaternion built from exact-arithmetic
// components (0, 0.6, 0, 0.8 — 0.36 + 0.64 == 1) rotating a base vector.
// Returns a pooled Vec3; the caller resets the pools at end of tick.
export function tickImpulse(): Vec3 {
    const spin = quatPool.take().set(0, 0.6, 0, 0.8);
    const base = vec3Pool.take().set(0.001, 0, 0.002);
    return spin.rotate(vec3Pool.take(), base);
}
