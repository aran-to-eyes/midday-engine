// m1-exit Phase 3 (CONCERNS #12a) exit test: KNOWN 'midday'-qualified type
// annotations extract through EVERY call site typeFromAnnotation serves —
// field annotation (midday.Vec3), method parameter (both spellings:
// import('midday').EntityRef and midday.Vec3), and method return type —
// so dropping the engine-d.ts gate from any one site fails THIS fixture,
// not just the others (council P3, gpt finding G3: each threaded site
// needs its own falsifiable pin). The checker vouches these names exist
// (module 'midday' binds); extraction owns the TypeDesc mapping and must
// emit entity_ref / vec3 — no silent component drop, no fabrication.
import {Component, component, field} from 'midday'

@component()
export class Seeker extends Component {
    @field({ min: 0 }) range = 5;
    @field() home: midday.Vec3 = { x: 0, y: 0, z: 0 };

    aim(target: import('midday').EntityRef, offset: midday.Vec3) {
        void target;
        void offset;
    }

    rest(): midday.Vec3 {
        return { x: 0, y: 0, z: 0 };
    }
}
