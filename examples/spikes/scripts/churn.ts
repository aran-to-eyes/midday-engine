// examples/spikes/scripts/churn.ts — the kata's per-tick agent workload
// (m0-determinism-spike). Every fixed update builds a burst of short-lived
// arrays and objects (REAL GC pressure: the run host's cumulative alloc
// counter must move — `.exercised.ts_gc_churn`), folds two seeded Philox
// streams through it (`__midday_rng`, the SIM profile's answer to the
// poisoned Math.random), and journals the result as kata.churn. A byte-
// identical journal across hosts therefore proves the TS allocator, the
// RNG streams, and the emit seam replay together — per tick, not once.

declare function __midday_emit(event: string, payload: unknown, key: string): void
declare function __midday_rng(stream: string): number

interface Grain {
  id: number
  weight: number
  trail: number[]
}

export default class Cycle {
  onFixedUpdate(dt: number): void {
    const load = 24 + Math.floor(__midday_rng('churn.load') * 24)
    const grains: Grain[] = []
    for (let i = 0; i < load; i++) {
      grains.push({ id: i, weight: __midday_rng('churn.weight'), trail: [i * dt, i * i * dt] })
    }
    const sum = grains
      .map((g) => g.weight * (g.trail[0] + 1))
      .filter((w) => w > 0.01)
      .reduce((a, b) => a + b, 0)
    __midday_emit('kata.churn', { load: load, sum: sum }, 'self')
  }
}
