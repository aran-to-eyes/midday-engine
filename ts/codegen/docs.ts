// ts/codegen/docs.ts — api_docs.md emitter: the generated reference agents
// read. Layout spec: api/CODEGEN.md "api_docs.md layout". Paragraphs are
// joined by exactly one blank line; doc text is flattened to one line; table
// cells escape '|' — but dump()ed default cells stay verbatim (spec'd).

import { JObject, JValue, dumpJson, findKey } from "./json";
import { cellEscape, entries, str, text, truthy } from "./model";

// Doc text as one markdown line (newlines -> spaces); empty stays empty.
function flat(doc: string): string {
    return doc.split("\n").join(" ");
}

function tick(value: string): string {
    return "`" + value + "`";
}

// `- level: `x`` / `- base: `x`` / `- compat_hash: `x``, one paragraph.
function meta(entry: JValue, withLevel: boolean): string {
    let out = "";
    if (withLevel) out += "- level: " + tick(str(entry, "level")) + "\n";
    const base = findKey(entry, "base");
    if (base !== null && base.k === "str") out += "- base: " + tick(base.v) + "\n";
    return out + "- compat_hash: " + tick(str(entry, "compat_hash"));
}

// `name(a: float, b: int = 5) -> ret` (docs signature spelling).
function signature(holder: JValue): string {
    const params = entries(holder, "params").map((param) => {
        const fallback = findKey(param, "default");
        return (
            str(param, "name") + ": " + str(param, "type") +
            (fallback !== null ? " = " + dumpJson(fallback) : "")
        );
    });
    return str(holder, "name") + "(" + params.join(", ") + ") -> " + str(holder, "returns");
}

function row(cells: string[]): string {
    return "|" + cells.map((cell) => " " + cell + " |").join("");
}

function table(header: string[], rows: string[]): string {
    const lines = [row(header), row(header.map(() => "---"))].concat(rows);
    return lines.join("\n");
}

function pushDocParagraph(paragraphs: string[], entry: JValue, key: string): void {
    const doc = text(entry, key);
    if (doc !== "") paragraphs.push(flat(doc));
}

function dumpCell(holder: JValue): string {
    const fallback = findKey(holder, "default");
    return fallback !== null ? tick(dumpJson(fallback)) : "";
}

function classesSection(out: string[], document: JObject): void {
    out.push("## Classes");
    const classes = entries(document, "classes");
    if (classes.length === 0) {
        out.push("_None registered._");
        return;
    }
    for (const entry of classes) {
        out.push("### " + tick(str(entry, "name")));
        pushDocParagraph(out, entry, "doc");
        out.push(meta(entry, true));

        const properties = entries(entry, "properties");
        if (properties.length === 0) {
            out.push("_No properties._");
        } else {
            const rows = properties.map((property) => {
                let flags = "";
                const list = findKey(property, "flags");
                if (list !== null && list.k === "arr")
                    for (const flag of list.v)
                        if (flag.k === "str") flags += (flags === "" ? "" : ", ") + flag.v;
                return row([
                    tick(str(property, "name")),
                    tick(str(property, "type")),
                    dumpCell(property),
                    flags,
                    cellEscape(text(property, "doc")),
                ]);
            });
            out.push(table(["property", "type", "default", "flags", "doc"], rows));
        }

        const methods = entries(entry, "methods");
        if (methods.length === 0) {
            out.push("_No methods._");
        } else {
            out.push("Methods:");
            const list = methods.map((method) => {
                const doc = text(method, "doc");
                return (
                    "- " + tick(signature(method)) + " (compat_hash " +
                    tick(str(method, "compat_hash")) + ")" +
                    (doc !== "" ? " -- " + flat(doc) : "")
                );
            });
            out.push(list.join("\n"));
        }
    }
}

function eventsSection(out: string[], document: JObject): void {
    out.push("## Events");
    const events = entries(document, "events");
    if (events.length === 0) {
        out.push("_None registered._");
        return;
    }
    for (const entry of events) {
        out.push("### " + tick(str(entry, "name")));
        pushDocParagraph(out, entry, "doc");
        out.push(meta(entry, true));
        const payload = entries(entry, "payload");
        if (payload.length === 0) {
            out.push("_No payload._");
            continue;
        }
        const rows = payload.map((field) =>
            row([tick(str(field, "name")), tick(str(field, "type")), cellEscape(text(field, "doc"))]),
        );
        out.push(table(["field", "type", "doc"], rows));
    }
}

function functionsSection(out: string[], document: JObject): void {
    out.push("## Expression functions");
    const functions = entries(document, "functions");
    if (functions.length === 0) {
        out.push("_None registered._");
        return;
    }
    for (const entry of functions) {
        out.push("### " + tick(signature(entry)));
        pushDocParagraph(out, entry, "doc");
        out.push(meta(entry, true));
    }
}

function verbsSection(out: string[], document: JObject): void {
    out.push("## CLI verbs");
    const verbs = entries(document, "verbs");
    if (verbs.length === 0) {
        out.push("_None registered._");
        return;
    }
    for (const entry of verbs) {
        out.push("### " + tick("midday " + str(entry, "name")));
        pushDocParagraph(out, entry, "summary");
        out.push(meta(entry, false));

        const flags = entries(entry, "flags");
        if (flags.length === 0) {
            out.push("_No flags._");
        } else {
            out.push("Flags:");
            const rows = flags.map((flag) =>
                row([
                    tick("--" + str(flag, "name")),
                    tick(str(flag, "type")),
                    truthy(flag, "required") ? "yes" : "no",
                    dumpCell(flag),
                    cellEscape(text(flag, "doc")),
                ]),
            );
            out.push(table(["flag", "type", "required", "default", "doc"], rows));
        }

        const positionals = entries(entry, "positionals");
        if (positionals.length === 0) {
            out.push("_No positionals._");
        } else {
            out.push("Positionals:");
            const rows = positionals.map((positional) =>
                row([
                    tick(str(positional, "name")),
                    tick(str(positional, "type")),
                    truthy(positional, "required") ? "yes" : "no",
                    truthy(positional, "variadic") ? "yes" : "no",
                    cellEscape(text(positional, "doc")),
                ]),
            );
            out.push(table(["positional", "type", "required", "variadic", "doc"], rows));
        }
    }
}

export function emitDocs(document: JObject): string {
    const paragraphs: string[] = [];
    paragraphs.push("# Midday Engine API reference");
    // Generator-neutral provenance (byte-identical bootstrap/selfhost output).
    paragraphs.push(
        "GENERATED from engine_api.json. DO NOT EDIT.\n" +
            "Signature compat hashes are XXH3-64 over signature-only JSON (docs excluded).",
    );
    paragraphs.push(
        "- engine_version: " + tick(str(document, "engine_version")) +
            "\n- api_compat_hash: " + tick(str(document, "api_compat_hash")),
    );
    classesSection(paragraphs, document);
    eventsSection(paragraphs, document);
    functionsSection(paragraphs, document);
    verbsSection(paragraphs, document);
    return paragraphs.join("\n\n") + "\n";
}
