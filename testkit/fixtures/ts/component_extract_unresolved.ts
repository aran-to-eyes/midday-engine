// m1-exit Phase 3 (CONCERNS #12a) negative exit test, SCHEMA-owned refusal:
// the NAMESPACE spelling `midday.TriggerEnteredEvent` passes the checker
// (the ambient namespace declares every engine interface, events included)
// and reaches the extraction walk — which must refuse FAIL-CLOSED with
// schema.unresolved_type: the name exists in engine.d.ts but the field-type
// table maps no event payloads until the M2 event-vocabulary decision
// (CONCERNS #12b). Never a fabricated manifest row, never a silent drop.
// (The import('midday') spelling of the same name dies EARLIER, at the
// module surface — component_extract_module_surface.ts pins that.)
import {Component, component, field} from 'midday'

@component()
export class Broken extends Component {
    @field({ min: 0 }) power = 1;

    onTouch(ev: midday.TriggerEnteredEvent) {
        void ev;
    }
}
