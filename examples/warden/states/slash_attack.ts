// State script — the state's brain. Sequence timing lives in the machine YAML
// (trigger/span tracks); this script only orchestrates what data can't express.
import {StateScript, Animator} from 'midday'

export default class SlashAttack extends StateScript {
  onEnter(from: string) {
    this.entity.get(Animator).play('slash', {blend: 0.1})
  }
  // No onExit cleanup needed: the HitboxLive span closes deterministically inside
  // the exit chain (Appendix A) whether the sequence completes or is interrupted.
}
