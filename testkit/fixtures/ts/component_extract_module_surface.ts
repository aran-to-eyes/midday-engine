// M2 0B (#12b) POSITIVE exit test, MODULE-surface event vocabulary —
// consciously flipped from the m1-exit Phase 3 negative it retired:
// `declare module "midday"` now re-exports the `...Event`-suffixed payload
// aliases alongside the unsuffixed ones, so the SPEC-LITERAL two-param
// listener below both typechecks AND extracts. The corpus gate pins the
// exact manifest event_bindings pair {event, payload_compat_hash} —
// resolved against bindings_spec.json's generated event_payload_types
// bijection, never reconstructed from the type name.
import {Component, component, field} from 'midday'

@component()
export class Eavesdropper extends Component {
    @field({ min: 0 }) sensitivity = 1;

    onEvent(event: 'trigger.entered', payload: import('midday').TriggerEnteredEvent): void {
        void event;
        void payload;
    }
}
