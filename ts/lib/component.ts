// midday/component — the code-first component authoring surface (spec §7 /
// §4.2, m1-ts-components). This file is the REAL RUNTIME implementation —
// ts/lib/index.ts re-exports it as the RUNTIME resolution target for the
// bare "midday" specifier (ts/toolchain/toolchain.cpp's module resolver).
// It is NOT what typechecks `import ... from 'midday'`, though: tsc always
// prefers an in-program AMBIENT module declaration over paths-based file
// resolution for an EXACT specifier match (confirmed empirically), so
// api/engine.d.ts's `declare module "midday" { ... }` (api/CODEGEN.md
// "Script component API") is a SEPARATE, parallel type declaration for the
// SAME public shape — kept in sync with this file BY HAND (a documented,
// accepted coupling; see ts/toolchain/README.md). The ONE piece the two
// share for real is the per-event bare payload-type aliases, which
// reference the reflected `midday.EventPayloads` namespace directly and so
// can never drift.
//
// Decorators are METADATA SHAPE ONLY: @component()/@field() exist so
// authored classes typecheck and so the shape is unambiguous to read, but
// the actual SCHEMA (field name/type/default/constraints) is derived by
// ts/toolchain/driver.js walking the source AST — the decorators below are
// NEVER consulted for that (exit test: extraction never executes the
// component). At real sim runtime they are near-no-ops (tagging only).
//
// entity.get/tryGet/has read a per-entity component DIRECTORY that lives
// right here as module-local state (never `globalThis` — unlike the
// state-script seat, nothing outside this module needs to reach it by a
// fixed global name; __attachComponent is the one seam a loader/test uses
// to populate it, since "no code-assembled entities" keeps ordinary game
// code from ever calling it). TS-authored components have no C++ type, so
// they can never live in a typed ecs::Pool<T> (core/ecs/world.h aborts on
// an unregistered type by design) — the directory is genuinely the only
// place their data can live.

export interface FieldOptions {
    min?: number;
    max?: number;
    save?: boolean;
    event?: boolean;
}

declare function __midday_entity_status(
    index: number,
    generation: number,
): { alive: boolean; despawn_tick: number | null };
declare function __midday_entity_root(
    index: number,
    generation: number,
): { index: number; generation: number };
declare function __midday_trigger_entity(
    event: string,
    payload: Record<string, unknown>,
    index: number,
    generation: number,
): number;
declare function __midday_trigger_named(
    event: string,
    payload: Record<string, unknown>,
    name: string,
): number;
declare function __midday_world_spawn(
    prefab: string,
    at: { x: number; y: number; z: number } | null,
    overrides: Record<string, Record<string, unknown>>,
): { index: number; generation: number };
declare function __midday_world_despawn(index: number, generation: number): void;

// index -> { generation, components: {name -> instance} }. Slot reuse
// (core/ecs/entity.h, LIFO) means a bucket must be re-validated against the
// CURRENT generation on every access — see EntityRef.bucket() below; a
// stale bucket left behind by a despawned incarnation is never trusted.
const directory = new Map<number, { generation: number; components: Record<string, unknown> }>();

/** Loader/test seam (never a game-facing export path): associate a live
 *  component instance with an entity. `entity.get/tryGet/has` are the only
 *  read side; there is deliberately no public spawn/attach API for game
 *  code (scripts/check_entity_api.py: entities are born from data). */
export function __attachComponent(
    index: number,
    generation: number,
    name: string,
    instance: unknown,
): void {
    let slot = directory.get(index);
    if (slot === undefined || slot.generation !== generation) {
        slot = { generation, components: Object.create(null) as Record<string, unknown> };
        directory.set(index, slot);
    }
    slot.components[name] = instance;
}

// Thrown by get/tryGet/root on a dead handle — a PLAIN throw (never routed
// through a native host function) so the resulting exception carries a real
// file:line (ts/runtime/component_host.h explains why in detail: a throw
// raised INSIDE a native function sits behind a stack frame
// script_runtime.cpp's converter cannot locate). The despawn tick and
// entity identity travel as a stable, greppable substring — the same
// convention m1-events-format's bus.payload_invalid halt already uses,
// because structured `details` never survive the host -> JS throw boundary.
function staleRefError(index: number, generation: number, despawnTick: number | null, site: string): Error {
    const when = despawnTick === null ? "an unknown tick" : "tick " + despawnTick;
    return new Error(
        "script.stale_ref: " + site + " on a despawned entity (index=" + index +
            " generation=" + generation + "): despawned at " + when +
            "; despawn_tick=" + (despawnTick === null ? "null" : String(despawnTick)),
    );
}

export interface ComponentCtor<T extends Component> {
    new (): T;
    readonly name: string;
}

export class EntityRef {
    constructor(
        readonly index: number,
        readonly generation: number,
    ) {}

    get alive(): boolean {
        return __midday_entity_status(this.index, this.generation).alive;
    }

    private liveComponents(site: string): Record<string, unknown> {
        const status = __midday_entity_status(this.index, this.generation);
        if (!status.alive) throw staleRefError(this.index, this.generation, status.despawn_tick, site);
        const slot = directory.get(this.index);
        return slot !== undefined && slot.generation === this.generation
            ? slot.components
            : Object.create(null);
    }

    get<T extends Component>(ctor: ComponentCtor<T>): T {
        const value = this.liveComponents("get(" + ctor.name + ")")[ctor.name];
        if (value === undefined)
            throw new Error(
                "script.missing_component: entity#" + this.index + " has no " + ctor.name,
            );
        return value as T;
    }

    tryGet<T extends Component>(ctor: ComponentCtor<T>): T | undefined {
        return this.liveComponents("tryGet(" + ctor.name + ")")[ctor.name] as T | undefined;
    }

    has(ctor: ComponentCtor<Component>): boolean {
        const status = __midday_entity_status(this.index, this.generation);
        if (!status.alive) return false;
        const slot = directory.get(this.index);
        return (
            slot !== undefined &&
            slot.generation === this.generation &&
            Object.prototype.hasOwnProperty.call(slot.components, ctor.name)
        );
    }

    root(): EntityRef {
        const status = __midday_entity_status(this.index, this.generation);
        if (!status.alive) throw staleRefError(this.index, this.generation, status.despawn_tick, "root()");
        const r = __midday_entity_root(this.index, this.generation);
        return new EntityRef(r.index, r.generation);
    }
}

export const events = {
    /** spec 4.2: symbolic keys resolve at the entity/channel the caller
     *  already holds — an EntityRef is the entity-private channel, a string
     *  is a named/well-known broadcast (e.g. "global") or project group. */
    trigger(name: string, payload: Record<string, unknown>, opts: { key: EntityRef | string }): void {
        if (typeof opts.key === "string") __midday_trigger_named(name, payload, opts.key);
        else __midday_trigger_entity(name, payload, opts.key.index, opts.key.generation);
    },
};

export abstract class Component {
    // Populated by __attachComponent, once, before any script observes the
    // instance — never reassigned after. `readonly` in engine.d.ts's
    // ambient EntityRef-consuming surface; plain here because this file IS
    // the seam that sets it.
    entity!: EntityRef;

    /** Sugar (spec 4.2): `this.emit(name, payload)` == `events.trigger(name, payload, {key: this.entity})`. */
    emit(name: string, payload: Record<string, unknown> = {}): void {
        events.trigger(name, payload, { key: this.entity });
    }
}

/** Every entity's invariant local TRS (spec 4.1: "Base components ...
 *  Transform ... are always live"). A fixed, engine-owned component — not
 *  `@component()`-authored — declared here (not engine_api.json's
 *  reflected "classes", which m1-scene-format's loader wiring owns) so the
 *  authoring surface has a concrete, real value to `entity.get()`. */
export class Transform extends Component {
    position: midday.Vec3 = { x: 0, y: 0, z: 0 };
    rotation: midday.Quat = { x: 0, y: 0, z: 0, w: 1 };
    scale: midday.Vec3 = { x: 1, y: 1, z: 1 };
}

export abstract class StateScript {
    entity!: EntityRef;

    emit(name: string, payload: Record<string, unknown> = {}): void {
        events.trigger(name, payload, { key: this.entity });
    }

    onEnter?(from: string): void;
    onExit?(to: string): void;
    onUpdate?(dt: number): void;
    onFixedUpdate?(dt: number): void;
}

// One TypeScript (TC39, stage-3) decorator pair. No transform, no
// experimentalDecorators — @component()/@field() exist for the class
// AUTHOR's shape and for driver.js's AST walk, not for anything they
// themselves run (see the file header).
export function component(): (
    ctor: new (...args: never[]) => Component,
    context: ClassDecoratorContext,
) => void {
    return () => {};
}

export function field(
    _options?: FieldOptions,
): (value: undefined, context: ClassFieldDecoratorContext) => void {
    return () => {};
}

type ComponentsOf<T extends readonly ComponentCtor<Component>[]> = {
    [K in keyof T]: T[K] extends ComponentCtor<infer C> ? C : never;
};

export const world = {
    /** Typed multi-component join, active rows only (spec 4.1 query
     *  semantics: systems use this for "ordered global logic over ECS
     *  queries", spec §7). Deliberately the OBJECT tier, not the SoA
     *  `midday/batch` tier — see api/CODEGEN.md "Script component API" for
     *  why the two never share a mechanism. */
    *query<T extends readonly ComponentCtor<Component>[]>(
        ...ctors: T
    ): IterableIterator<[EntityRef, ...ComponentsOf<T>]> {
        for (const [index, slot] of directory) {
            const status = __midday_entity_status(index, slot.generation);
            if (!status.alive) continue;
            const values: unknown[] = [];
            let matched = true;
            for (const ctor of ctors) {
                const value = slot.components[ctor.name];
                if (value === undefined) {
                    matched = false;
                    break;
                }
                values.push(value);
            }
            if (matched)
                yield [new EntityRef(index, slot.generation), ...values] as unknown as [
                    EntityRef,
                    ...ComponentsOf<T>,
                ];
        }
    },

    /** Prefab-ONLY spawning (spec §7 "no code-assembled entities"): `prefab`
     *  names a resolved `*.entity.yaml` file — the SAME instancing +
     *  override mechanism scenes/machines already use (m1-scene-format) —
     *  never a caller-assembled component list (there is no other way to
     *  bring an entity into existence from script). `at` is the entity's
     *  local translation; `overrides` is the SAME `<machine>/<Region>/...`
     *  property-diff grammar an authored `prefab: ... override:` block uses.
     *  Queues through the deferred structural queue (native seat:
     *  ts/runtime/world_host.h -> core/loader/prefab_spawn.h): the returned
     *  handle reads `.alive === false` until the tick's structural-apply
     *  phase (Appendix A.1 phase 8), when the prefab's machines' enter
     *  chains run and it goes live. */
    spawn(
        prefab: string,
        opts?: { at?: midday.Vec3; overrides?: Record<string, Record<string, unknown>> },
    ): EntityRef {
        const at = opts?.at ?? null;
        const atArg = at !== null ? { x: at.x, y: at.y, z: at.z } : null;
        const r = __midday_world_spawn(prefab, atArg, opts?.overrides ?? {});
        return new EntityRef(r.index, r.generation);
    },

    /** Queues a despawn through the SAME deferred structural queue every
     *  despawn rides — `ref` stays alive until the structural-apply phase. */
    despawn(ref: EntityRef): void {
        __midday_world_despawn(ref.index, ref.generation);
    },
};
