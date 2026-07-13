// M2 0B (#12b) negative exit test, literal/payload-type mismatch refusal:
// the onEvent shape is right, but the literal event name and the payload
// type disagree — TriggerEnteredEvent is trigger.entered's payload, not
// contact.began's (bindings_spec.json event_payload_types is the truth).
// tsc cannot see this (the two types are structurally unrelated but the
// signature is well-formed), so the extraction walk must refuse with the
// DISTINCT schema.event_mismatch.
import {Component, component, field} from 'midday'

@component()
export class CrossedWires extends Component {
    @field({ min: 0 }) gain = 1;

    onEvent(event: 'contact.began', payload: import('midday').TriggerEnteredEvent): void {
        void event;
        void payload;
    }
}
