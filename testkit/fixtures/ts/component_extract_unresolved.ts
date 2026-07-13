// m1-exit Phase 3 (CONCERNS #12a) negative exit test, SCHEMA-owned refusal:
// a NAMESPACE spelling that passes the checker (the ambient namespace
// declares every engine interface) but that the field-type table maps no
// row for — the extraction walk must refuse FAIL-CLOSED with
// schema.unresolved_type. Never a fabricated manifest row, never a silent
// drop. Retargeted at M2 0B (#12b): the original TriggerEnteredEvent
// annotation became the DISTINCT payload-position refusal
// (schema.event_payload_param, component_extract_event_param.ts pins it),
// so this fixture now uses the EventPayloads lookup-map interface — d.ts
// membership without a table row, the same fail-closed class as before.
import {Component, component, field} from 'midday'

@component()
export class Broken extends Component {
    @field({ min: 0 }) power = 1;

    onTouch(ev: midday.EventPayloads) {
        void ev;
    }
}
