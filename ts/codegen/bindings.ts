// ts/codegen/bindings.ts — bindings_spec.json emitter: the glue spec
// m0-batch-bindings implements. Shape spec: api/CODEGEN.md
// "bindings_spec.json layout". No subsystem ever gets hand-written bindings.
//
// The batch envelope (D-BUILD-069), the state-script hook seam
// (D-BUILD-084), and the event-payload bijection (M2 #12b) are SELF-HOST
// ONLY: the retired-in-place bootstrap emitter stays frozen on the
// version-0 placeholder, and the byte-equivalence gate compares
// bindings_spec.json modulo these members
// (selfhost::bindings_equivalence_view).

import { JObject, JValue, dumpJson, findKey, jArr, jInt, jObj, jStr } from "./json";
import { entries, pascalCase, str } from "./model";

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

// The GENERATED event-payload bijection (M2 #12b, SELF-HOST ONLY like the
// two seams above): module `...Event` payload type name -> {event,
// payload_compat_hash}, input order — the map driver.js's onEvent
// extraction consults; the extractor never reconstructs event names or
// compat hashes from naming conventions.
function eventPayloadTypes(document: JObject): JValue {
    return jObj(
        entries(document, "events").map((entry): [string, JValue] => [
            pascalCase(str(entry, "name")) + "Event",
            jObj([
                ["event", jStr(str(entry, "name"))],
                ["payload_compat_hash", jStr(str(entry, "compat_hash"))],
            ]),
        ]),
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
        [
            "state_script_hooks",
            jObj([
                ["envelope_version", jInt(1)],
                ["register", jStr("__midday_register_state_script")],
                ["introspect", jStr("__midday_state_hooks_of")],
                ["invoke", jStr("__midday_invoke_state_hook")],
                ["emit", jStr("__midday_emit")],
                [
                    "hooks",
                    jArr([jStr("onEnter"), jStr("onExit"), jStr("onUpdate"), jStr("onFixedUpdate")]),
                ],
            ]),
        ],
        ["event_payload_types", eventPayloadTypes(document)],
    ]);
    return dumpJson(spec) + "\n";
}
