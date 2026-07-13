// ts/codegen/dts.ts — engine.d.ts emitter + the structural shape self-check
// (formats/engine_dts.meta.md). Layout spec: api/CODEGEN.md "engine.d.ts
// layout"; bytes pinned by the selfhost equivalence harness against the
// bootstrap goldens.

import { JObject, JValue, findKey } from "./json";
import { entries, jsdocEscape, pascalCase, str, text, truthy, tsType } from "./model";

// The fixed value-type preamble: one declaration per scalar TypeDesc
// spelling that is not a TypeScript primitive (mapping table in CODEGEN.md).
const VALUE_TYPES =
    '    /** TypeDesc "vec2": 2D float vector. */\n' +
    "    interface Vec2 {\n        x: number;\n        y: number;\n    }\n" +
    "\n" +
    '    /** TypeDesc "vec3": 3D float vector. */\n' +
    "    interface Vec3 {\n        x: number;\n        y: number;\n        z: number;\n    }\n" +
    "\n" +
    '    /** TypeDesc "vec4": 4D float vector. */\n' +
    "    interface Vec4 {\n        x: number;\n        y: number;\n        z: number;\n" +
    "        w: number;\n    }\n" +
    "\n" +
    '    /** TypeDesc "quat": rotation quaternion; JSON spelling [x, y, z, w]. */\n' +
    "    interface Quat {\n        x: number;\n        y: number;\n        z: number;\n" +
    "        w: number;\n    }\n" +
    "\n" +
    '    /** TypeDesc "color": linear RGBA; JSON spelling [r, g, b, a]. */\n' +
    "    interface Color {\n        r: number;\n        g: number;\n        b: number;\n" +
    "        a: number;\n    }\n" +
    "\n" +
    '    /** TypeDesc "entity_ref": generational entity handle; a stale handle reads alive == false. */\n' +
    "    interface EntityRef {\n" +
    "        readonly index: number;\n" +
    "        readonly generation: number;\n" +
    "        readonly alive: boolean;\n" +
    "        get<T extends import(\"midday\").Component>(ctor: import(\"midday\").ComponentCtor<T>): T;\n" +
    "        tryGet<T extends import(\"midday\").Component>(ctor: import(\"midday\").ComponentCtor<T>): T | undefined;\n" +
    "        has(ctor: import(\"midday\").ComponentCtor<import(\"midday\").Component>): boolean;\n" +
    "        root(): EntityRef;\n" +
    "    }\n" +
    "\n" +
    '    /** TypeDesc "asset_ref": project-root-relative asset path. */\n' +
    "    type AssetRef = string;\n";

// One JSDoc line at `indent`, or nothing when the doc is empty.
function jsdoc(doc: string, indent: string): string {
    return doc === "" ? "" : indent + "/** " + jsdocEscape(doc) + " */\n";
}

// `    interface <name> {}` or a full body; body lines are pre-indented.
function interfaceBlock(doc: string, name: string, body: string): string {
    const shell = body === "" ? " {}\n" : " {\n" + body + "    }\n";
    return jsdoc(doc, "    ") + "    interface " + name + shell;
}

// Interface member names are emitted bare only when they are valid TS
// identifiers; engine names also allow '.' and '-' (CODEGEN.md "Member
// quoting"). Function/method PARAMETER names cannot be quoted, stay verbatim.
function memberName(name: string): string {
    let bare = name !== "" && !(name[0] >= "0" && name[0] <= "9");
    for (let i = 0; i < name.length; ++i) {
        const c = name[i];
        bare =
            bare &&
            ((c >= "a" && c <= "z") || (c >= "A" && c <= "Z") || (c >= "0" && c <= "9") ||
                c === "_" || c === "$");
    }
    return bare ? name : '"' + name + '"';
}

// `name: T;` member line with optional JSDoc, 8-space indent.
function member(doc: string, declaration: string): string {
    return jsdoc(doc, "        ") + "        " + declaration + ";\n";
}

// `a: number, b?: number` — a param with a default becomes optional.
function paramList(holder: JValue): string {
    return entries(holder, "params")
        .map(
            (param) =>
                str(param, "name") +
                (findKey(param, "default") !== null ? "?: " : ": ") +
                tsType(str(param, "type")),
        )
        .join(", ");
}

// The `"<name>": <Type>;` lookup-map interface (keys always quoted).
function mapBlock(doc: string, mapName: string, rows: [string, string][]): string {
    let body = "";
    for (const [key, type] of rows) body += '        "' + key + '": ' + type + ";\n";
    return interfaceBlock(doc, mapName, body);
}

function classBlock(entry: JValue): string {
    let body = "";
    for (const property of entries(entry, "properties")) {
        let declaration = "";
        const flags = findKey(property, "flags");
        if (flags !== null && flags.k === "arr")
            for (const flag of flags.v)
                if (flag.k === "str" && flag.v === "read_only") declaration += "readonly ";
        declaration += memberName(str(property, "name")) + ": " + tsType(str(property, "type"));
        body += member(text(property, "doc"), declaration);
    }
    for (const method of entries(entry, "methods"))
        body += member(
            text(method, "doc"),
            str(method, "name") + "(" + paramList(method) + "): " + tsType(str(method, "returns")),
        );
    return interfaceBlock(text(entry, "doc"), pascalCase(str(entry, "name")), body);
}

function eventBlock(entry: JValue): string {
    let body = "";
    for (const field of entries(entry, "payload"))
        body += member(
            text(field, "doc"),
            memberName(str(field, "name")) + ": " + tsType(str(field, "type")),
        );
    return interfaceBlock(text(entry, "doc"), pascalCase(str(entry, "name")) + "Event", body);
}

function exprBlock(document: JObject): string {
    let body = "";
    for (const entry of entries(document, "functions"))
        body += member(
            text(entry, "doc"),
            "function " + str(entry, "name") + "(" + paramList(entry) + "): " +
                tsType(str(entry, "returns")),
        );
    if (body === "") return "    namespace expr {}\n";
    return "    namespace expr {\n" + body + "    }\n";
}

// The fixed component-authoring surface: api/CODEGEN.md "Script component
// API". Declared AMBIENTLY (never a real backing file) because tsc always
// prefers an in-program ambient module declaration over paths-based file
// resolution for an EXACT specifier match, so a competing "midday" paths
// entry would sit inert (proven empirically) — ts/lib/component.ts is the
// real runtime implementation this block must stay in sync with by hand
// (ts/toolchain/toolchain.cpp resolves "midday" to it at RUNTIME only).
const COMPONENT_API =
    "    export type Vec2 = midday.Vec2;\n" +
    "    export type Vec3 = midday.Vec3;\n" +
    "    export type Vec4 = midday.Vec4;\n" +
    "    export type Quat = midday.Quat;\n" +
    "    export type Color = midday.Color;\n" +
    "    export type EntityRef = midday.EntityRef;\n" +
    "    export type AssetRef = midday.AssetRef;\n" +
    "\n" +
    "    export interface FieldOptions {\n" +
    "        min?: number;\n" +
    "        max?: number;\n" +
    "        save?: boolean;\n" +
    "        event?: boolean;\n" +
    "    }\n" +
    "\n" +
    "    export interface ComponentCtor<T extends Component> {\n" +
    "        new (): T;\n" +
    "        readonly name: string;\n" +
    "    }\n" +
    "\n" +
    "    /** Entity-bound event subscription (M2 #12b): one binding per onEvent OVERLOAD DECLARATION — a literal event name paired with its ...Event payload type; a union-only signature carries no bindings and refuses. */\n" +
    "    export interface EventListener {\n" +
    "        onEvent(event: string, payload: unknown): void;\n" +
    "    }\n" +
    "\n" +
    "    export abstract class Component {\n" +
    "        readonly entity: midday.EntityRef;\n" +
    "        emit(name: string, payload?: Record<string, unknown>): void;\n" +
    "        /** State-scoped lifecycle (M2 #12b): the owning state's enter/exit chains invoke these. */\n" +
    "        onEnter?(from: string): void;\n" +
    "        onExit?(to: string): void;\n" +
    "    }\n" +
    "\n" +
    "    export abstract class StateScript {\n" +
    "        readonly entity: midday.EntityRef;\n" +
    "        emit(name: string, payload?: Record<string, unknown>): void;\n" +
    "        onEnter?(from: string): void;\n" +
    "        onExit?(to: string): void;\n" +
    "        onUpdate?(dt: number): void;\n" +
    "        onFixedUpdate?(dt: number): void;\n" +
    "    }\n" +
    "\n" +
    "    export class Transform extends Component {\n" +
    "        position: midday.Vec3;\n" +
    "        rotation: midday.Quat;\n" +
    "        scale: midday.Vec3;\n" +
    "    }\n" +
    "\n" +
    "    export function component(): (\n" +
    "        ctor: new (...args: any[]) => Component,\n" +
    "        context: ClassDecoratorContext,\n" +
    "    ) => void;\n" +
    "    export function field(\n" +
    "        options?: FieldOptions,\n" +
    "    ): (value: undefined, context: ClassFieldDecoratorContext) => void;\n" +
    "\n" +
    "    export const events: {\n" +
    "        trigger(\n" +
    "            name: string,\n" +
    "            payload: Record<string, unknown>,\n" +
    "            opts: { key: midday.EntityRef | string },\n" +
    "        ): void;\n" +
    "    };\n" +
    "\n" +
    "    export const world: {\n" +
    "        query<T extends readonly ComponentCtor<Component>[]>(\n" +
    "            ...ctors: T\n" +
    "        ): IterableIterator<\n" +
    "            [midday.EntityRef, ...{ [K in keyof T]: T[K] extends ComponentCtor<infer C> ? C : never }]\n" +
    "        >;\n" +
    "        spawn(\n" +
    "            prefab: AssetRef,\n" +
    "            opts?: { at?: midday.Vec3; overrides?: Record<string, Record<string, unknown>> },\n" +
    "        ): midday.EntityRef;\n" +
    "        despawn(ref: midday.EntityRef, opts?: { after?: number }): void;\n" +
    "    };\n";

// Two payload-type aliases per registered event. The bare flavor
// (`import('midday').TriggerEntered`) is the ergonomic, suffix-free
// spelling m1-ts-components introduced; the `...Event`-suffixed flavor
// (M2 #12b) is the SPEC-LITERAL payload spelling an `onEvent` overload
// declaration binds with — the same names bindings_spec.json's
// event_payload_types map keys, so the extractor and the authored surface
// share one vocabulary (section 3 stays untouched). Never collides: two
// events cannot already share a pascalCase(name) (section 3's existing
// uniqueness claim on "<Pascal>Event" implies uniqueness of the bare
// "<Pascal>" prefix too).
function eventAliasBlock(document: JObject): string {
    let bare = "";
    let suffixed = "";
    for (const entry of entries(document, "events")) {
        const lookup = ' = midday.EventPayloads["' + str(entry, "name") + '"];\n';
        bare += "    export type " + pascalCase(str(entry, "name")) + lookup;
        suffixed += "    export type " + pascalCase(str(entry, "name")) + "Event" + lookup;
    }
    if (bare === "") return "";
    return bare + "\n" + suffixed;
}

function verbBlock(entry: JValue): string {
    let body = "";
    for (const flag of entries(entry, "flags")) {
        const type = str(flag, "type");
        let declaration = memberName(str(flag, "name"));
        if (type === "bool") declaration += "?: boolean";
        else declaration += (truthy(flag, "required") ? ": " : "?: ") + tsType(type);
        body += member(text(flag, "doc"), declaration);
    }
    for (const positional of entries(entry, "positionals")) {
        let declaration = memberName(str(positional, "name"));
        if (truthy(positional, "variadic")) declaration += ": " + tsType(str(positional, "type")) + "[]";
        else
            declaration +=
                (truthy(positional, "required") ? ": " : "?: ") + tsType(str(positional, "type"));
        body += member(text(positional, "doc"), declaration);
    }
    return interfaceBlock(text(entry, "summary"), pascalCase(str(entry, "name")) + "VerbArgs", body);
}

export function emitDts(document: JObject): string {
    const blocks: string[] = [];
    blocks.push(
        "    // -- Value types (fixed preamble; scalar TypeDesc spellings map per api/CODEGEN.md) --\n",
    );
    blocks.push(VALUE_TYPES);

    blocks.push('    // -- Reflected classes (engine_api.json "classes", registration order) --\n');
    let rows: [string, string][] = [];
    for (const entry of entries(document, "classes")) {
        blocks.push(classBlock(entry));
        rows.push([str(entry, "name"), pascalCase(str(entry, "name"))]);
    }
    blocks.push(mapBlock("Class name -> reflected interface.", "Classes", rows));

    blocks.push('    // -- Event payloads (engine_api.json "events", registration order) --\n');
    rows = [];
    for (const entry of entries(document, "events")) {
        blocks.push(eventBlock(entry));
        rows.push([str(entry, "name"), pascalCase(str(entry, "name")) + "Event"]);
    }
    blocks.push(mapBlock("Event name -> payload type.", "EventPayloads", rows));

    blocks.push(
        '    // -- Expression functions (engine_api.json "functions"): expression-language ' +
            "signatures for editor tooling, not TS-callable --\n",
    );
    blocks.push(exprBlock(document));

    blocks.push(
        '    // -- CLI verbs (engine_api.json "verbs"): midday argv schemas as types, ' +
            "manifest order --\n",
    );
    rows = [];
    for (const entry of entries(document, "verbs")) {
        blocks.push(verbBlock(entry));
        rows.push([str(entry, "name"), pascalCase(str(entry, "name")) + "VerbArgs"]);
    }
    blocks.push(mapBlock("Verb name -> parsed-argument type.", "VerbArgsByName", rows));

    return (
        "// engine.d.ts -- GENERATED from engine_api.json. " +
        "DO NOT EDIT.\n// engine_version " +
        str(document, "engine_version") +
        ", api_compat_hash " +
        str(document, "api_compat_hash") +
        " (signatures only; docs excluded).\n" +
        "// Formatting rules + the TypeDesc -> TypeScript mapping table: api/CODEGEN.md.\n" +
        "// Structural (pre-tsc) validation conventions: formats/engine_dts.meta.md.\n\n" +
        "declare namespace midday {\n" +
        blocks.join("\n") +
        "}\n\n" +
        '// -- Script component API (ambient; ts/lib/component.ts is the real ' +
            "runtime surface kept in sync by hand — api/CODEGEN.md \"Script component " +
            'API") --\n' +
        'declare module "midday" {\n' +
        COMPONENT_API +
        "\n" +
        eventAliasBlock(document) +
        "}\n"
    );
}

// Structural d.ts shape check (formats/engine_dts.meta.md): balanced braces
// on non-comment lines, every declared entry present, no unresolved-
// generation tokens. Empty result == shape-valid; failure = codegen.selfcheck.
export function dtsShapeErrors(dts: string, document: JObject): string[] {
    const errors: string[] = [];
    const tokens = ["TODO", "FIXME", "XXX", "PLACEHOLDER"];
    let depth = 0;
    const lines = dts.split("\n");
    for (let i = 0; i < lines.length; ++i) {
        const line = lines[i].replace(/^ +/, "");
        const comment = line.startsWith("//") || line.startsWith("/*");
        if (!comment) {
            for (const c of line) {
                depth += c === "{" ? 1 : 0;
                depth -= c === "}" ? 1 : 0;
                if (depth < 0) errors.push("line " + (i + 1) + ": unbalanced '}'");
            }
            for (const token of tokens)
                if (line.indexOf(token) !== -1)
                    errors.push("line " + (i + 1) + ": unresolved-generation token '" + token + "'");
        }
        if (depth < 0) break;
    }
    if (depth > 0) errors.push("unbalanced '{': " + depth + " unclosed");

    const need = (fragment: string): void => {
        if (dts.indexOf(fragment) === -1) errors.push("missing declaration fragment: " + fragment);
    };
    for (const entry of entries(document, "classes")) {
        need("interface " + pascalCase(str(entry, "name")) + " ");
        need('"' + str(entry, "name") + '":');
    }
    for (const entry of entries(document, "events")) {
        need("interface " + pascalCase(str(entry, "name")) + "Event ");
        need('"' + str(entry, "name") + '":');
    }
    for (const entry of entries(document, "functions")) need("function " + str(entry, "name") + "(");
    for (const entry of entries(document, "verbs")) {
        need("interface " + pascalCase(str(entry, "name")) + "VerbArgs ");
        need('"' + str(entry, "name") + '":');
    }
    return errors;
}
