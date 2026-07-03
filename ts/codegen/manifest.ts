// ts/codegen/manifest.ts — schema_manifest.json emitter: the
// validate-before-write source. Shape spec: api/CODEGEN.md
// "schema_manifest.json layout"; meta-schema:
// formats/schema_manifest.schema.json.

import { JObject, JValue, dumpJson, jArr, jInt, jObj, jStr } from "./json";
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
        ["formats", jArr([])], // scene/machine format schemas join at m1-scene-format
    ]);
    return dumpJson(manifest) + "\n";
}
