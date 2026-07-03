// testkit/fixtures/ts/naive_bench.ts — the comparison mode: the SAME sim as
// batch_bench.ts (state hashes must match) through per-entity, per-field
// JSON host hooks — the classic chatty embedding. Crossings per tick are
// 18N + 2: the number the batched mode beats by >= 10x (exit test).
// Stored values are re-read after every write so f32 rounding matches the
// typed-array path bit-for-bit.

import { onTick } from "midday/batch";
import { quatPool, vec3Pool } from "midday/math";
import { DECAY, DT, tickImpulse } from "./bench_impulse";

declare function __midday_naive_count(): number;
declare function __midday_naive_get(index: number, component: string, field: string): number;
declare function __midday_naive_set(
    index: number,
    component: string,
    field: string,
    value: number,
): void;

const AXES = ["x", "y", "z"];

onTick(() => {
    const impulse = tickImpulse();
    const accel = [impulse.x * DT, impulse.y * DT, impulse.z * DT];

    const n = __midday_naive_count();
    for (let i = 0; i < n; i++) {
        for (let k = 0; k < 3; k++) {
            const axis = AXES[k];
            const v = __midday_naive_get(i, "velocity", axis) + accel[k];
            __midday_naive_set(i, "velocity", axis, v);
            const p =
                __midday_naive_get(i, "position", axis) +
                __midday_naive_get(i, "velocity", axis) * DT;
            __midday_naive_set(i, "position", axis, p);
        }
        const cap = __midday_naive_get(i, "health", "max");
        const next = __midday_naive_get(i, "health", "current") - DECAY;
        __midday_naive_set(i, "health", "current", next < 0 ? cap : next);
    }

    vec3Pool.reset();
    quatPool.reset();
});
