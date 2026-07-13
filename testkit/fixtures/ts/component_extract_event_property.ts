// M2 0B council fix (C3) negative exit test, onEvent-as-property refusal:
// an onEvent authored as a class PROPERTY (the everyday arrow-function
// callback style) parses as a PropertyDeclaration, not a method — the
// extraction walk's overload reader sees METHOD declarations only, while
// the runtime introspection (typeof s.onEvent === "function") would still
// report the hook present: zero bindings, hook "present", silently never
// subscribed. The module typechecks (the base class declares no onEvent),
// so the refusal must come from the extraction walk as the structured
// schema.event_listener_shape.
import {Component, component} from 'midday'

@component()
export class Perky extends Component {
    onEvent = (event: string, payload: unknown): void => { void event; void payload; };
}
