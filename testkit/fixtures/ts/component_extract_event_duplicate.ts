// M2 0B (#12b) negative exit test, duplicate-binding refusal: two onEvent
// overload declarations bind the SAME event — dispatch order would be
// ambiguous and the manifest's event_bindings no longer a bijection. Legal
// TypeScript (identical overloads typecheck), so the extraction walk must
// refuse with the DISTINCT schema.event_duplicate.
import {Component, component, field} from 'midday'

@component()
export class DoubleAgent extends Component {
    @field({ min: 0 }) paranoia = 1;

    onEvent(event: 'trigger.entered', payload: import('midday').TriggerEnteredEvent): void;
    onEvent(event: 'trigger.entered', payload: import('midday').TriggerEnteredEvent): void;
    onEvent(event: 'trigger.entered', payload: import('midday').TriggerEnteredEvent): void {
        void event;
        void payload;
    }
}
