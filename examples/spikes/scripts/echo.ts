// examples/spikes/scripts/echo.ts — the cascade rung (m0-determinism-spike):
// Excited is entered by kata.beat the tick the drive sequence fires it
// (cascade depth 1); this onEnter immediately emits kata.echo (depth 2),
// which flips the tally region in the SAME tick — three regions moved by
// one authored keyframe. The string churn is deliberate allocation noise on
// the enter path; charge is a seeded draw that lands in the journal.

declare function __midday_emit(event: string, payload: unknown, key: string): void
declare function __midday_rng(stream: string): number

export default class Excited {
  onEnter(from: string): void {
    const charge = __midday_rng('echo.charge')
    const shards: string[] = []
    for (let i = 0; i < 8; i++) {
      shards.push(from + ':' + i.toString(16) + ':' + charge.toFixed(6))
    }
    void shards.join('|')
    __midday_emit('kata.echo', { charge: charge }, 'self')
  }
}
