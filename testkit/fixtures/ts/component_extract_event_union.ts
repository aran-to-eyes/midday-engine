// M2 0B (#12b) negative exit test, union-only refusal: a lone union
// implementation signature loses the exact event<->payload bijection (which
// payload goes with which event?). Multi-event listeners spell one overload
// DECLARATION per event above a union implementation; with no declarations
// at all, the extraction walk must refuse with the DISTINCT
// schema.event_union_only.
import {Component, component, field} from 'midday'

@component()
export class Scattershot extends Component {
    @field({ min: 0 }) spread = 1;

    onEvent(
        event: 'trigger.entered' | 'trigger.exited',
        payload: import('midday').TriggerEnteredEvent | import('midday').TriggerExitedEvent,
    ): void {
        void event;
        void payload;
    }
}
