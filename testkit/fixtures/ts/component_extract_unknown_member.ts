// m1-exit Phase 3 (CONCERNS #12a) negative exit test, TYPE-owned refusal:
// a 'midday'-qualified name that does not exist at all dies one pass
// EARLIER than the schema walk — engine.d.ts binds module 'midday', so the
// checker itself refuses (TS2694, script.type_error) before extraction
// runs. Pinned so a d.ts/module-binding change that silently demotes this
// to `any` (reopening the fabrication window) breaks the gate loudly.
import {Component, component, field} from 'midday'

@component()
export class Typoed extends Component {
    @field({ min: 0 }) power = 1;

    strike(target: import('midday').Nonexistent) {
        void target;
    }
}
