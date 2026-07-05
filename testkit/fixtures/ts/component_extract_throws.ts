// m1-ts-components exit test #2: this component's top-level code would
// EMIT (a real side effect) and then THROW if the module were ever
// executed. Schema extraction (ts/toolchain/driver.js) reads the
// @component()/@field() shape straight from the AST — a static walk over
// every top-level statement, order-independent — and must still extract
// `Dangerous` cleanly, proving the code is never run.
import {Component, component, field, events} from 'midday'

function detonate(): never {
    events.trigger("component_extract_throws.detonated", {}, { key: "global" });
    throw new Error("component_extract_throws: this module must never execute");
}

detonate();

@component()
export class Dangerous extends Component {
    @field({ min: 0 }) power = 10;
    @field() label = "dangerous";
}
