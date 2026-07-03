// testkit/fixtures/ts/batch_bench.ts — the batched-mode budget fixture
// (`midday script bench`, m0-batch-bindings): integrate velocity into
// position and decay health over typed-array batch views. Boundary
// crossings per tick: 8 buffer refreshes + 7 writable commits + 1 tick
// entry — constant in the entity count. Steady-state GC bytes: zero
// (pooled math, no per-tick object creation).

import { onTick, request } from "midday/batch";
import { quatPool, vec3Pool } from "midday/math";
import { DECAY, DT, tickImpulse } from "./bench_impulse";

// One request = one aligned join: row i is the same entity in both views.
const kinematics = request({
    components: [
        { component: "position", fields: ["x", "y", "z"] },
        { component: "velocity", fields: ["x", "y", "z"] },
    ],
});
const vitals = request({
    components: [{ component: "health", fields: ["current", "max"] }],
});

onTick(() => {
    const impulse = tickImpulse();
    const ax = impulse.x * DT;
    const ay = impulse.y * DT;
    const az = impulse.z * DT;

    // Re-read buffers at tick entry (midday/batch protocol: growth detaches
    // stale ArrayBuffers deterministically).
    const pos = kinematics.views[0];
    const vel = kinematics.views[1];
    const px = pos.buffers.x;
    const py = pos.buffers.y;
    const pz = pos.buffers.z;
    const vx = vel.buffers.x;
    const vy = vel.buffers.y;
    const vz = vel.buffers.z;
    const n = pos.count;
    for (let i = 0; i < n; i++) {
        vx[i] += ax;
        vy[i] += ay;
        vz[i] += az;
        px[i] += vx[i] * DT;
        py[i] += vy[i] * DT;
        pz[i] += vz[i] * DT;
    }

    const health = vitals.views[0];
    const hp = health.buffers.current;
    const cap = health.buffers.max; // read_only column: never scattered back
    const m = health.count;
    for (let i = 0; i < m; i++) {
        const next = hp[i] - DECAY;
        hp[i] = next < 0 ? cap[i] : next;
    }

    vec3Pool.reset();
    quatPool.reset();
});
