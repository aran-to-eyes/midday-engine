// State-scoped component: lives on the Hurtbox entity under the HitboxLive state node —
// only receives events/ticks while the span holds it active.
import {Component, component, field, events} from 'midday'
import {Health} from './health'

@component()
export class DamageOnTouch extends Component {
  @field() amount = 40
  @field() stagger = 0

  // [SPEC-GAP #6: built-in engine event vocabulary (trigger.entered, contact.began, ...)
  //  is implied by tick phase 6 but never enumerated — inventing `trigger.entered` here]
  onEvent(ev: import('midday').TriggerEntered) {
    const hp = ev.other.tryGet(Health)
    if (!hp) return
    hp.damage(this.amount, this.entity.root())   // attribute damage to the Warden, not the hurtbox
    if (this.stagger > 0) {
      events.trigger('stagger.hit', {force: this.stagger}, {key: ev.other})
    }
  }
}
