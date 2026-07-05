// ts/codegen/manifest.ts — schema_manifest.json emitter: the
// validate-before-write source. Shape spec: api/CODEGEN.md
// "schema_manifest.json layout"; meta-schema:
// formats/schema_manifest.schema.json.

import { JObject, JValue, dumpJson, jArr, jBool, jInt, jObj, jStr } from "./json";
import { entries, str } from "./model";

// The fixed spelling table, TypeKind declaration order — one row per
// TypeDesc spelling, its JSON wire shape mirroring reflect::TypeDesc::accepts.
const VALUE_TYPES: [string, string, number][] = [
    ["bool", "boolean", 0],
    ["int", "integer", 0],
    ["float", "number", 0],
    ["string", "string", 0],
    ["name", "string", 0],
    ["vec2", "number_tuple", 2],
    ["vec3", "number_tuple", 3],
    ["vec4", "number_tuple", 4],
    ["quat", "number_tuple", 4],
    ["color", "number_tuple", 4],
    ["entity_ref", "string", 0],
    ["asset_ref", "string", 0],
    ["array", "array_of_element", 0],
    ["map", "object_of_element", 0],
];

// `formats[]` (m1-scene-format, manifest.ts:73's reserved slot,
// D-BUILD-106/108): the scene/machine/entity format-entry schemas
// core/loader/format_schema.h's GENERIC engine validates against
// (`midday validate --schema scene|machine|entity`). Hardcoded DATA here —
// exactly like VALUE_TYPES above — never derived from `document` (the
// reflected engine_api.json): these are loader-owned document SHAPES, not
// reflected runtime types. A scalar field (no `kind`) is a TypeDesc
// spelling; `kind: "object"` / `"array_of_object"` fields carry their own
// nested `fields` (empty = opaque — that SHAPE is required, contents
// unchecked; format_schema.h's "necessary but not sufficient" call: the
// REAL semantic authority stays core/loader/scene_load.cpp /
// machine_load.cpp / entity_load.cpp, which every consuming verb
// (`midday run`, `midday scene print`) actually loads through).
interface FieldDef {
    name: string;
    type?: string;
    required?: boolean;
    enumValues?: string[];
    kind?: "object" | "array_of_object";
    fields?: FieldDef[];
}

interface FormatDef {
    name: string;
    currentVersion: number;
    fields: FieldDef[];
}

function fieldToJson(field: FieldDef): JValue {
    const pairs: [string, JValue][] = [["name", jStr(field.name)]];
    if (field.kind !== undefined) {
        pairs.push(["kind", jStr(field.kind)]);
        pairs.push(["fields", jArr((field.fields ?? []).map(fieldToJson))]);
    } else {
        pairs.push(["type", jStr(field.type ?? "string")]);
    }
    if (field.required) pairs.push(["required", jBool(true)]);
    if (field.enumValues !== undefined) pairs.push(["enum", jArr(field.enumValues.map(jStr))]);
    return jObj(pairs);
}

function formatToJson(format: FormatDef): JValue {
    return jObj([
        ["name", jStr(format.name)],
        ["current_version", jInt(format.currentVersion)],
        ["fields", jArr(format.fields.map(fieldToJson))],
    ]);
}

// An opaque array-of-object field: must be that shape, contents unchecked
// (a component/machine/attachment list's polymorphic entries — the generic
// engine's flat vocabulary cannot describe an open-ended `{Name: {...}}`
// union without becoming a second schema language, format_schema.h).
function opaqueList(name: string): FieldDef {
    return { name, kind: "array_of_object", fields: [] };
}

function opaqueObject(name: string, required?: boolean): FieldDef {
    return { name, kind: "object", fields: [], required };
}

const FORMAT_ENTRIES: FormatDef[] = [
    {
        name: "scene",
        currentVersion: 1,
        fields: [
            { name: "scene", type: "name", required: true },
            { name: "events", type: "array<string>" },
            {
                name: "entities",
                kind: "array_of_object",
                fields: [
                    { name: "entity", type: "name", required: true },
                    opaqueList("components"),
                    opaqueList("machines"),
                    opaqueObject("prefab"),
                    { name: "at", type: "array<float>" },
                    opaqueObject("override"),
                ],
            },
        ],
    },
    {
        name: "machine",
        currentVersion: 1,
        fields: [
            { name: "machine", type: "name", required: true },
            opaqueObject("vars"),
            opaqueObject("regions", true),
        ],
    },
    {
        name: "entity",
        currentVersion: 1,
        fields: [
            { name: "entity", type: "name", required: true },
            opaqueList("base"),
            opaqueList("machines"),
            opaqueList("attachments"),
        ],
    },
];

function valueTypes(): JValue {
    return jArr(
        VALUE_TYPES.map(([spelling, json, size]) => {
            const row: [string, JValue][] = [
                ["spelling", jStr(spelling)],
                ["json", jStr(json)],
            ];
            if (size !== 0) row.push(["size", jInt(size)]);
            return jObj(row);
        }),
    );
}

export function emitManifest(document: JObject): string {
    const events = entries(document, "events").map((entry) =>
        jObj([
            ["name", jStr(str(entry, "name"))],
            [
                "payload",
                jArr(
                    entries(entry, "payload").map((field) =>
                        jObj([
                            ["name", jStr(str(field, "name"))],
                            ["type", jStr(str(field, "type"))],
                        ]),
                    ),
                ),
            ],
        ]),
    );

    const functions = entries(document, "functions").map((entry) =>
        jObj([
            ["name", jStr(str(entry, "name"))],
            ["params", jArr(entries(entry, "params").map((param) => jStr(str(param, "type"))))],
            ["returns", jStr(str(entry, "returns"))],
        ]),
    );

    const manifest = jObj([
        ["format_version", jInt(1)],
        ["api_compat_hash", jStr(str(document, "api_compat_hash"))],
        ["value_types", valueTypes()],
        ["events", jArr(events)],
        ["expr_functions", jArr(functions)],
        ["formats", jArr(FORMAT_ENTRIES.map(formatToJson))],
    ]);
    return dumpJson(manifest) + "\n";
}
