// ts/codegen/bindings.ts — bindings_spec.json emitter: the glue spec
// m0-batch-bindings implements. Shape spec: api/CODEGEN.md
// "bindings_spec.json layout". No subsystem ever gets hand-written bindings.

import { JObject, JValue, dumpJson, findKey, jArr, jInt, jObj, jStr } from "./json";
import { str } from "./model";

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

const ENVELOPE_DOC =
    "Reserved: m0-batch-bindings designs the real batch envelope (per-query SoA " +
    "views backed by typed arrays, one segment per component column, pooled math " +
    "slots, per-tick crossing/GC counters). views stays empty and envelope_version " +
    "stays 0 until then; refuse envelope_version 0 for actual batching.";

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
                ["envelope_version", jInt(0)],
                ["status", jStr("placeholder")],
                ["doc", jStr(ENVELOPE_DOC)],
                ["views", jArr([])],
            ]),
        ],
    ]);
    return dumpJson(spec) + "\n";
}
