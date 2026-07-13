// M2 0B (#12b) negative exit test, payload-in-ordinary-method refusal: an
// event payload type is a SUBSCRIPTION vocabulary, legal only as an onEvent
// overload's second parameter. Here it typechecks fine (the module exports
// the suffixed aliases since #12b) but names a payload in a plain method's
// parameter list — the extraction walk must refuse with the DISTINCT
// schema.event_payload_param, never demote it to a generic unresolved type
// and never fabricate a manifest row.
import {Component, component, field} from 'midday'

@component()
export class Bystander extends Component {
    @field({ min: 0 }) alertness = 1;

    hear(ev: import('midday').ContactBeganEvent) {
        void ev;
    }
}
