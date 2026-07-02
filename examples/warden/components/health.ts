// Code-first component — decorated fields ARE the schema (validate-before-write derives from this).
import {Component, component, field, EntityRef, Transform} from 'midday'

@component()
export class Health extends Component {
  @field({min: 0}) max = 100
  @field() value = 100

  damage(amount: number, by: EntityRef) {
    if (this.value <= 0) return
    this.value = Math.max(0, this.value - amount)
    // [SPEC-GAP #7: `this.emit()` = trigger at own entity key — sugar implied, never specced]
    this.emit('damage.dealt', {amount, by, point: this.entity.get(Transform).position})
    if (this.value === 0) this.emit('death.dealt', {by})
  }
}
