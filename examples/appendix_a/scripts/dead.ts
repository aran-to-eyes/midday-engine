// examples/appendix_a/scripts/dead.ts — the Dead state's brain (Appendix
// A.3): entering Dead broadcasts the kill on the GLOBAL channel — the
// trace's depth-2 cascade (UI/score listeners react; no transition; the
// combat region is already marked this tick, so nothing re-enters).
//
// __midday_emit is the state-script emit seam the run host provides
// (loader::resolve_key spellings: self | root | global | <group>); full
// onEnter wiring — and the cause chain contact -> damage -> death.dealt ->
// transition -> boss.died — is proven at m0-appendix-a-determinism.

declare function __midday_emit(event: string, payload: unknown, key: string): void

export default class Dead {
  onEnter(from: string): void {
    void from // A.3: always SlashAttack in the worked trace
    __midday_emit('boss.died', {}, 'global')
  }
}
