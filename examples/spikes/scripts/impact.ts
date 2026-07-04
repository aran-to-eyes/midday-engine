// examples/spikes/scripts/impact.ts — the physics tail
// (m0-determinism-spike): Jolt's phase-6 contact dispatch lands
// contact.began on the Agent's channel, the impact region enters Struck,
// and this onEnter broadcasts kata.impact on the GLOBAL channel with a
// seeded draw — the full contact -> transition -> script -> bus chain in
// one journaled cause line. onExit exercises the exit-hook crossing when
// the recovery sequence re-arms Watch.

declare function __midday_emit(event: string, payload: unknown, key: string): void
declare function __midday_rng(stream: string): number

export default class Struck {
  onEnter(from: string): void {
    void from
    __midday_emit('kata.impact', { jolt: __midday_rng('impact.jolt') }, 'global')
  }

  onExit(to: string): void {
    void to // nothing to tear down — the sequence owns the recovery window
  }
}
