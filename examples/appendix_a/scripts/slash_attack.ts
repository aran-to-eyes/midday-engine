// examples/appendix_a/scripts/slash_attack.ts — the SlashAttack state's
// brain (Appendix A.3). The swing's TIMING lives in the machine's dope
// sheet — trigger attack.swoosh @0.30s, span HitboxLive [0.40, 0.80] —
// data, not code; the brain only orchestrates what data cannot express.
//
// A.3 exit chain, tick 3200 (death.dealt wins the any-state rule):
//   1. slash_attack.ts onExit(Dead)   <- this hook, parts still live
//   2. open span HitboxLive closes -> Hurtbox subtree exits/deactivates
//   3. SlashAttack components onExit (reverse attach)
//   4. playhead reset (no history)
// No cleanup code needed here: the span closes deterministically inside
// the exit chain whether the sequence completes or death interrupts it.
// Hook invocation parity with the C++ fixture is proven at
// m0-appendix-a-determinism; this module loads on the SIM runtime today.

export default class SlashAttack {
  onEnter(from: string): void {
    void from // animation cue arrives with the Animator tier (m4)
  }

  onExit(to: string): void {
    void to // nothing to tear down — the exit chain owns the span
  }
}
