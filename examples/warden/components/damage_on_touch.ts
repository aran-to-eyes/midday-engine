// State-scoped component: lives on the Hurtbox entity under the HitboxLive state node —
// only receives events/ticks while the span holds it active.
import {Component, component, field, events} from 'midday'
import {Health} from './health'

@component()
export class DamageOnTouch extends Component {
  @field() amount = 40
  @field() stagger = 0

  // M2 0B (#12b): the spec-literal two-param listener shape — a literal
  // event name paired with its generated ...Event payload type; `midday
  // script extract` reads this overload into the manifest's event_bindings.
  onEvent(event: 'trigger.entered', payload: import('midday').TriggerEnteredEvent) {
    const hp = payload.other.tryGet(Health)
    if (!hp) return
    hp.damage(this.amount, this.entity.root())   // attribute damage to the Warden, not the hurtbox
    if (this.stagger > 0) {
      events.trigger('stagger.hit', {force: this.stagger}, {key: payload.other})
    }
  }
}
