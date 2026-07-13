// M2 0B (#12b) negative exit test, payload-as-@field refusal: event
// payloads are subscriptions, never component STATE — storing one in a
// @field would smuggle a transient event snapshot into the persistent
// schema. The annotation typechecks (the module exports the suffixed
// aliases since #12b), so the refusal must come from the extraction walk
// as the DISTINCT schema.event_payload_field.
import {Component, component, field} from 'midday'

@component()
export class Hoarder extends Component {
    @field() last!: import('midday').ContactBeganEvent;
}
