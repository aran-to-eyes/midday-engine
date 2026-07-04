// examples/spikes/tainted/scripts/tainted_clock.ts — deliberately TAINTED
// (m0-determinism-spike): Date.now() is wall clock, the one thing a SIM
// script may never read. This file is TYPE-clean on purpose — the only
// thing standing between it and the tick loop is the engine lint pack
// (no-wall-clock), and the spike.tainted tests pin that it fires through
// the real `midday run` path with exit 3 before any tick executes.

export default class Sample {
  onEnter(from: string): void {
    void from
    const started = Date.now() // <- the violation the lint gate must catch
    void started
  }
}
