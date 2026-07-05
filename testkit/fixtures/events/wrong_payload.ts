// testkit/fixtures/events/wrong_payload.ts — Trigger's onEnter emits the
// declared probe.hit event (payload: {amount: float}) with a STRING
// amount: the bus's typed validation (core/bus/bus.cpp) refuses at trigger
// time, the refusal throws into the script, and the run host halts with a
// structured, tick-annotated error (m1-events-format exit-test #3).
declare function __midday_emit(event: string, payload: unknown, key: string): void

export default class Trigger {
  onEnter(from: string): void {
    void from
    __midday_emit('probe.hit', { amount: 'oops' }, 'self')
  }
}
