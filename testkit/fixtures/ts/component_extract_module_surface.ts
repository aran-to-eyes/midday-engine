// m1-exit Phase 3 (CONCERNS #12a) negative exit test, MODULE-surface
// refusal: `declare module "midday"` (engine.d.ts:429) re-exports the value
// types, the authoring surface, and the UNSUFFIXED event-payload aliases
// (TriggerEntered = EventPayloads["trigger.entered"], ...) — but NOT the
// Event-suffixed interfaces, which live solely in the ambient NAMESPACE.
// So import('midday').TriggerEnteredEvent names a type the namespace
// declares but the module does not export, and the checker refuses it
// (TS2694) before extraction runs. Pinned because this is the exact module
// surface the M2 event-vocabulary decision (#12b) will deliberately extend
// — when it does, THIS fixture must be retired consciously, not silently.
import {Component, component, field} from 'midday'

@component()
export class Eavesdropper extends Component {
    @field({ min: 0 }) sensitivity = 1;

    overhear(ev: import('midday').TriggerEnteredEvent) {
        void ev;
    }
}
