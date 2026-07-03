// ts/codegen/generator.ts — the REAL engine code generator
// (m0-codegen-selfhost): engine_api.json text -> engine.d.ts +
// schema_manifest.json + api_docs.md + bindings_spec.json, byte-identical
// to the bootstrap corpus goldens (api/CODEGEN.md is the byte contract;
// the equivalence gate pins it against tools/codegen_bootstrap until that
// tool retires post-M0).
//
// Host surface (ts/codegen/host.d.ts, wired by selfhost.cpp): readInput()
// for the document text, writeOutput(name, content) per artifact, log(...)
// for debugging. One entry point: __midday_codegen_run(request) ->
// {ok:true, api_compat_hash, files} | {ok:false, error:{code,message,details}}.

import { emitBindings } from "./bindings";
import { dtsShapeErrors, emitDts } from "./dts";
import { emitDocs } from "./docs";
import { JObject, JsonParseError } from "./json";
import { emitManifest } from "./manifest";
import { CodegenError, loadDocument, str } from "./model";

interface Artifact {
    name: string;
    content: string;
}

function generate(document: JObject): Artifact[] {
    const dts = emitDts(document);
    // Post-generation structural self-check (formats/engine_dts.meta.md):
    // a failure here is a generator bug, never bad input.
    const shape = dtsShapeErrors(dts, document);
    if (shape.length > 0)
        throw new CodegenError(
            "codegen.selfcheck",
            "generated engine.d.ts failed its structural shape check",
            { errors: shape },
        );
    return [
        { name: "engine.d.ts", content: dts },
        { name: "schema_manifest.json", content: emitManifest(document) },
        { name: "api_docs.md", content: emitDocs(document) },
        { name: "bindings_spec.json", content: emitBindings(document) },
    ];
}

function requestOrigin(request: unknown): string {
    if (typeof request === "object" && request !== null) {
        const origin = (request as Record<string, unknown>)["origin"];
        if (typeof origin === "string" && origin !== "") return origin;
    }
    return "<input>";
}

function run(request: unknown): unknown {
    try {
        const document = loadDocument(readInput(), requestOrigin(request));
        const artifacts = generate(document);
        for (const artifact of artifacts) writeOutput(artifact.name, artifact.content);
        return {
            ok: true,
            api_compat_hash: str(document, "api_compat_hash"),
            files: artifacts.map((artifact) => artifact.name),
        };
    } catch (thrown) {
        if (thrown instanceof CodegenError)
            return {
                ok: false,
                error: { code: thrown.code, message: thrown.message, details: thrown.details },
            };
        if (thrown instanceof JsonParseError)
            return {
                ok: false,
                error: {
                    code: "json.parse",
                    message: thrown.message,
                    details: {
                        file: thrown.origin,
                        line: thrown.line,
                        col: thrown.col,
                    },
                },
            };
        // Anything else escaping is a generator bug (exit class 1, like
        // codegen.selfcheck) — never misreported as bad input.
        const message =
            thrown instanceof Error && thrown.stack !== undefined
                ? thrown.message + "\n" + thrown.stack
                : String(thrown);
        return { ok: false, error: { code: "codegen.internal", message } };
    }
}

globalThis.__midday_codegen_run = run;

export {};
