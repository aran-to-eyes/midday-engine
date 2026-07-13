// examples/lifecycle/components/lifecycle_components.ts — the
// component_event_lifecycle golden's script components (M2 node 0B,
// FUSED-SPEC D6). One module, five components; `midday script extract`
// turns this file into examples/lifecycle/lifecycle.components.json (the
// behavioral gate regenerates and byte-compares it — manifest drift reds).
//
// ContactRelay (base, on the probe) is the typed-hydration witness: its
// spec-literal two-param onEvent verifies the canonical projection the bus
// dispatched — self hydrates to ITS OWN entity, other to a DISTINCT ref,
// position to a real Vec3 whose authored -0.0 y arrived normalized (+0),
// impulse's -0.0 likewise — plus the MIRRORED base Transform read
// (entity.get(Transform).position.x === expectedX). It journals every
// verdict as relay.verify, then emits golden.kill ONLY when the mapping
// held (the self/other-swap falsifier refuses here; the signed-zero
// falsifier deliberately does NOT — float canonicalization is the byte
// assertion's job, so the kill still fires and exit/reap stay green).
//
// ParentExitA/B + ChildExitA/B are the exit-chain markers: state-scoped,
// hook-only — their statechart.hook component_exit records pin the A.2.1
// reverse-attach exit order (the golden's 7-line chain).
import {Component, Transform, component, field} from 'midday'

@component()
export class ContactRelay extends Component {
  @field() expectedX = 0

  onEvent(event: 'contact.began', payload: import('midday').ContactBeganEvent) {
    const me = this.entity
    const selfIsMe =
      payload.self.index === me.index && payload.self.generation === me.generation
    const otherDistinct =
      payload.other.index !== payload.self.index ||
      payload.other.generation !== payload.self.generation
    const tx = this.entity.get(Transform).position.x
    this.emit('relay.verify', {
      event,
      self_is_me: selfIsMe,
      other_distinct: otherDistinct,
      x: payload.position.x,
      y_plus_zero: Object.is(payload.position.y, 0), // false for -0 (Object.is)
      z: payload.position.z,
      impulse_plus_zero: Object.is(payload.impulse, 0),
      tx,
    })
    if (selfIsMe && otherDistinct && payload.position.x === this.expectedX &&
        payload.position.z === 3 && tx === this.expectedX) {
      this.emit('golden.kill', {})
    }
  }

  onExit(to: string) {
    void to // the despawn path's base onExit -> ONE component.despawn_exit record
  }
}

@component()
export class ParentExitA extends Component {
  onExit(to: string) {
    void to // golden exit-chain line 6 (outer level, reverse attach)
  }
}

@component()
export class ParentExitB extends Component {
  onExit(to: string) {
    void to // golden exit-chain line 5
  }
}

@component()
export class ChildExitA extends Component {
  onExit(to: string) {
    void to // golden exit-chain line 4
  }
}

@component()
export class ChildExitB extends Component {
  onExit(to: string) {
    void to // golden exit-chain line 3 (deepest level exits first)
  }
}
