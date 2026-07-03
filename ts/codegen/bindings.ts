// ts/codegen/bindings.ts — bindings_spec.json emitter: the glue spec
// m0-batch-bindings implements. Shape spec: api/CODEGEN.md
// "bindings_spec.json layout". No subsystem ever gets hand-written bindings.
//
// The batch envelope here is SELF-HOST ONLY (D-BUILD-069): the retired-in-
// place bootstrap emitter stays frozen on the version-0 placeholder, and the
// byte-equivalence gate compares bindings_spec.json modulo this one member.

import { JObject, JValue, dumpJson, findKey, jArr, jInt, jObj, jStr } from "./json";
import { entries, str } from "./model";

// Deep copy minus every doc/summary key: signatures stay verbatim (level,
// defaults, flags, compat hashes IN), prose stays out of the glue contract.
function stripDocs(value: JValue): JValue {
    if (value.k === "obj")
        return jObj(
            value.v
                .filter(([key]) => key !== "doc" && key !== "summary")
                .map(([key, item]): [string, JValue] => [key, stripDocs(item)]),
        );
    if (value.k === "arr") return jArr(value.v.map(stripDocs));
    return value;
}

// Batchable columns: TypeDesc spelling -> [buffer element kind, width]
// (api/CODEGEN.md "bindings_spec.json layout"). Reference and composite
// types (string, name, entity_ref, asset_ref, array, map) never batch —
// they stay on the structured seam.
const BATCH_COLUMNS = new Map<string, [buffer: string, width: number]>([
    ["bool", ["u8", 1]],
    ["int", ["f64", 1]],
    ["float", ["f32", 1]],
    ["vec2", ["f32", 2]],
    ["vec3", ["f32", 3]],
    ["vec4", ["f32", 4]],
    ["quat", ["f32", 4]],
    ["color", ["f32", 4]],
]);

// One view template per class, input order; fields keep property declaration
// order and drop non-batchable types. writable = the read_only flag is
// absent (the runtime never scatters read_only columns back).
function batchViews(document: JObject): JValue {
    return jArr(
        entries(document, "classes").map((cls) => {
            const fields: JValue[] = [];
            for (const property of entries(cls, "properties")) {
                const spelling = str(property, "type");
                const column = BATCH_COLUMNS.get(spelling);
                if (column === undefined) continue;
                const readOnly = entries(property, "flags").some(
                    (flag) => flag.k === "str" && flag.v === "read_only",
                );
                fields.push(
                    jObj([
                        ["name", jStr(str(property, "name"))],
                        ["type", jStr(spelling)],
                        ["buffer", jStr(column[0])],
                        ["width", jInt(column[1])],
                        ["writable", { k: "bool", v: !readOnly }],
                    ]),
                );
            }
            return jObj([
                ["component", jStr(str(cls, "name"))],
                ["fields", jArr(fields)],
            ]);
        }),
    );
}

export function emitBindings(document: JObject): string {
    const section = (key: string): JValue => {
        const value = findKey(document, key);
        return stripDocs(value === null ? jArr([]) : value);
    };
    const spec = jObj([
        ["format_version", jInt(1)],
        ["api_compat_hash", jStr(str(document, "api_compat_hash"))],
        ["expr_functions", section("functions")],
        ["events", section("events")],
        ["classes", section("classes")],
        [
            "batch_envelope",
            jObj([
                ["envelope_version", jInt(1)],
                ["views", batchViews(document)],
            ]),
        ],
    ]);
    return dumpJson(spec) + "\n";
}
