// ts/codegen/model.ts — document accessors, the shared text rules
// (pascal_case, TypeDesc -> TypeScript, JSDoc/cell escapes), and the full
// validation walk of api/CODEGEN.md "Validation order": envelope unwrap,
// api::check_document parity, then the type-spelling / entry-shape /
// generated-symbol-uniqueness walk. Emitters never see garbage.

import { JObject, JValue, dumpJson, findKey, parseJson } from "./json";

// Structured failure carried to the host: same codes and exit classes as
// the bootstrap tool (json.parse | api.malformed | codegen.*).
export class CodegenError extends Error {
    constructor(
        readonly code: string,
        message: string,
        readonly details: Record<string, unknown> = {},
    ) {
        super(message);
    }
}

function malformedDocument(problem: string): never {
    throw new CodegenError("api.malformed", "engine_api document: " + problem);
}

function malformedEntry(section: string, entry: string, problem: string): never {
    throw new CodegenError(
        "codegen.malformed",
        "section '" + section + "' entry '" + entry + "': " + problem,
        { section, entry, problem },
    );
}

// ------------------------------------------------------------- accessors
// Every accessor presumes the shape loadDocument validated (emit_util.h twin).

export function entries(parent: JValue, key: string): JValue[] {
    const value = findKey(parent, key);
    return value !== null && value.k === "arr" ? value.v : [];
}

export function str(obj: JValue, key: string): string {
    const value = findKey(obj, key);
    return value !== null && value.k === "str" ? value.v : "";
}

// Optional doc/summary text: empty when absent (absent and empty emit alike).
export function text(obj: JValue, key: string): string {
    return str(obj, key);
}

export function truthy(obj: JValue, key: string): boolean {
    const value = findKey(obj, key);
    return value !== null && value.k === "bool" && value.v;
}

// ------------------------------------------------------------- text rules

// Split on '.', '_', '-'; uppercase the first ASCII letter of each segment.
export function pascalCase(name: string): string {
    let out = "";
    let boundary = true;
    for (let i = 0; i < name.length; ++i) {
        const c = name[i];
        if (c === "." || c === "_" || c === "-") {
            boundary = true;
            continue;
        }
        out += boundary && c >= "a" && c <= "z" ? String.fromCharCode(c.charCodeAt(0) - 32) : c;
        boundary = false;
    }
    return out;
}

function replaceAll(input: string, from: string, to: string): string {
    return input.split(from).join(to);
}

export function jsdocEscape(doc: string): string {
    return replaceAll(replaceAll(doc, "\n", " "), "*/", "*\\/");
}

export function cellEscape(doc: string): string {
    return replaceAll(replaceAll(doc, "\n", " "), "|", "\\|");
}

// ---------------------------------------------------------- TypeDesc rules

const SCALAR_TS = new Map<string, string>([
    ["bool", "boolean"],
    ["int", "number"],
    ["float", "number"],
    ["string", "string"],
    ["name", "string"],
    ["vec2", "Vec2"],
    ["vec3", "Vec3"],
    ["vec4", "Vec4"],
    ["quat", "Quat"],
    ["color", "Color"],
    ["entity_ref", "EntityRef"],
    ["asset_ref", "AssetRef"],
]);

// reflect::TypeDesc::parse parity: scalar spelling, array<T>, map<T>.
export function isTypeSpelling(spelling: string): boolean {
    const open = spelling.indexOf("<");
    if (open === -1) return SCALAR_TS.has(spelling);
    const head = spelling.slice(0, open);
    if (head !== "array" && head !== "map") return false;
    if (spelling.length < open + 2 || !spelling.endsWith(">")) return false;
    return isTypeSpelling(spelling.slice(open + 1, spelling.length - 1));
}

// Pre: isTypeSpelling(spelling) — validation walks first, exactly like the
// native ts_type over a parsed TypeDesc.
export function tsType(spelling: string): string {
    const open = spelling.indexOf("<");
    if (open === -1) {
        const mapped = SCALAR_TS.get(spelling);
        if (mapped === undefined)
            throw new CodegenError("codegen.internal", "tsType on unvalidated '" + spelling + "'");
        return mapped;
    }
    const element = tsType(spelling.slice(open + 1, spelling.length - 1));
    return spelling.startsWith("array") ? element + "[]" : "Record<string, " + element + ">";
}

// ------------------------------------------------------------- validation

const HEX64 = /^[0-9a-f]{16}$/;
const SECTIONS = ["classes", "events", "functions", "verbs"] as const;

// api::check_document parity (format 1).
function checkDocument(document: JValue): asserts document is JObject {
    if (document.k !== "obj") malformedDocument("not a JSON object");
    const format = findKey(document, "format_version");
    if (format === null || format.k !== "int") malformedDocument("missing integer format_version");
    if (format.raw !== "1")
        malformedDocument("unknown format_version " + dumpJson(format) + " (this build reads 1)");
    const version = findKey(document, "engine_version");
    if (version === null || version.k !== "str") malformedDocument("missing string engine_version");
    const hash = findKey(document, "api_compat_hash");
    if (hash === null || hash.k !== "str" || !HEX64.test(hash.v))
        malformedDocument("api_compat_hash is not 16-digit lowercase hex");
    for (const section of SECTIONS) {
        const list = findKey(document, section);
        if (list === null || list.k !== "arr")
            malformedDocument("missing array section '" + section + "'");
        const names: string[] = [];
        const context = "section '" + section + "': ";
        for (const entry of list.v) {
            if (entry.k !== "obj") malformedDocument(context + "entry is not an object");
            const name = findKey(entry, "name");
            if (name === null || name.k !== "str" || name.v === "")
                malformedDocument(context + "entry without a name");
            const compat = findKey(entry, "compat_hash");
            if (compat === null || compat.k !== "str" || !HEX64.test(compat.v))
                malformedDocument(
                    context + "entry '" + name.v + "' compat_hash is not 16-digit lowercase hex",
                );
            if (names.indexOf(name.v) !== -1)
                malformedDocument(context + "duplicate entry '" + name.v + "'");
            names.push(name.v);
        }
    }
}

// A "type"-carrying key must hold a canonical TypeDesc spelling.
function checkSpelling(section: string, entry: string, holder: JValue, key: string): void {
    const spelling = findKey(holder, key);
    if (spelling === null || spelling.k !== "str")
        malformedEntry(section, entry, "missing string '" + key + "'");
    if (!isTypeSpelling(spelling.v))
        throw new CodegenError(
            "codegen.unknown_type",
            "section '" + section + "' entry '" + entry + "': unknown type spelling '" +
                spelling.v + "'",
            { section, entry, type: spelling.v },
        );
}

// A field list: an array of objects, each with a string name + valid type.
function checkFields(section: string, entry: string, holder: JValue, key: string): void {
    const fields = findKey(holder, key);
    if (fields === null || fields.k !== "arr")
        malformedEntry(section, entry, "missing array '" + key + "'");
    for (const field of fields.v) {
        const name = findKey(field, "name");
        if (field.k !== "obj" || name === null || name.k !== "str")
            malformedEntry(section, entry, key + " item without a string name");
        checkSpelling(section, entry, field, "type");
    }
}

// Everything the emitters read beyond checkDocument's guarantees, plus
// cross-file uniqueness of every generated TypeScript symbol.
function checkGenerationModel(document: JObject): void {
    const symbols = [
        "Vec2", "Vec3", "Vec4", "Quat", "Color", "EntityRef", "AssetRef",
        "expr", "Classes", "EventPayloads", "VerbArgsByName",
    ];
    const claim = (symbol: string): void => {
        if (symbols.indexOf(symbol) !== -1)
            throw new CodegenError(
                "codegen.duplicate_symbol",
                "generated TypeScript symbol '" + symbol +
                    "' collides (api/CODEGEN.md naming rules)",
                { symbol },
            );
        symbols.push(symbol);
    };

    for (const entry of entries(document, "classes")) {
        const name = str(entry, "name");
        checkFields("classes", name, entry, "properties");
        const methods = findKey(entry, "methods");
        if (methods === null || methods.k !== "arr")
            malformedEntry("classes", name, "missing array 'methods'");
        for (const method of methods.v) {
            const methodName = findKey(method, "name");
            if (method.k !== "obj" || methodName === null || methodName.k !== "str")
                malformedEntry("classes", name, "method without a string name");
            checkFields("classes", name, method, "params");
            checkSpelling("classes", name, method, "returns");
        }
        claim(pascalCase(name));
    }
    for (const entry of entries(document, "events")) {
        const name = str(entry, "name");
        checkFields("events", name, entry, "payload");
        claim(pascalCase(name) + "Event");
    }
    for (const entry of entries(document, "functions")) {
        const name = str(entry, "name");
        checkFields("functions", name, entry, "params");
        checkSpelling("functions", name, entry, "returns");
    }
    for (const entry of entries(document, "verbs")) {
        const name = str(entry, "name");
        checkFields("verbs", name, entry, "flags");
        checkFields("verbs", name, entry, "positionals");
        claim(pascalCase(name) + "VerbArgs");
    }
}

// bytes -> validated format-1 document; unwraps `midday api dump --json`
// envelopes (top-level object without format_version but with object "api").
export function loadDocument(bytes: string, origin: string): JObject {
    let document = parseJson(bytes, origin);
    if (document.k === "obj" && findKey(document, "format_version") === null) {
        const payload = findKey(document, "api");
        if (payload !== null && payload.k === "obj") document = payload;
    }
    checkDocument(document);
    checkGenerationModel(document);
    return document;
}
