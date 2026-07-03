// ts/toolchain/driver.js — first-party compiler driver, evaluated on the
// embedded QuickJS right after the vendored typescript.js (global `ts`).
// One entry point: globalThis.__midday_ts_run(request) -> response.
//
//   request:  { entry, engine_dts, lib_dir, options, emit }
//   response: { diagnostics: [{kind, code, file, line, col, message}...],
//               js?, failure? }   // failure = infrastructure, not code
//
// All file access goes through the __midday_read_file host hook (C++ enforces
// the canonical-path allowlist). Lints are AST-level — a TypeScript syntax
// walk, never regex over text — and run on every non-declaration source in
// the program, whether or not the banned name typechecks. Bypass policy:
// none (ts/toolchain/toolchain.h).
"use strict";

(() => {
    const readCache = new Map();
    const read = (file) => {
        if (!readCache.has(file))
            readCache.set(file, __midday_read_file(file));
        return readCache.get(file);
    };

    const createHost = (options, libDir) => {
        const sources = new Map();
        return {
            fileExists: (f) => read(f) !== null,
            readFile: (f) => read(f) ?? undefined,
            getSourceFile(f, languageVersion) {
                if (!sources.has(f)) {
                    const text = read(f);
                    sources.set(f, text === null
                        ? undefined
                        : ts.createSourceFile(f, text, languageVersion, true));
                }
                return sources.get(f);
            },
            getDefaultLibFileName: () => libDir + "/lib.es2020.d.ts",
            getDefaultLibLocation: () => libDir,
            writeFile: () => {},
            getCurrentDirectory: () => "",
            getCanonicalFileName: (f) => f,
            useCaseSensitiveFileNames: () => true,
            getNewLine: () => "\n",
        };
    };

    const typeDiagnostic = (d) => {
        const out = {
            kind: "type",
            code: "TS" + d.code,
            file: "",
            line: 0,
            col: 0,
            message: ts.flattenDiagnosticMessageText(d.messageText, " "),
        };
        if (d.file && d.start !== undefined) {
            const pos = d.file.getLineAndCharacterOfPosition(d.start);
            out.file = d.file.fileName;
            out.line = pos.line + 1;
            out.col = pos.character + 1;
        }
        return out;
    };

    // Engine lint pack midday-lint/1. Unwraps a leading `globalThis.` so
    // `globalThis.Date.now()` and `Date.now()` are the same violation.
    const lintSourceFile = (sf, push) => {
        const flag = (node, code, what, why) => {
            const pos = sf.getLineAndCharacterOfPosition(node.getStart(sf));
            push({
                kind: "lint", code, file: sf.fileName,
                line: pos.line + 1, col: pos.character + 1,
                message: what + " is banned in sim code: " + why,
            });
        };
        const clock = "sim time is the tick, not the wall clock";
        const unGlobal = (e) =>
            ts.isPropertyAccessExpression(e) && ts.isIdentifier(e.expression) &&
            e.expression.text === "globalThis" ? e.name : e;
        const visit = (node) => {
            if (ts.isPropertyAccessExpression(node) && ts.isIdentifier(node.name)) {
                const base = unGlobal(node.expression);
                if (ts.isIdentifier(base)) {
                    const key = base.text + "." + node.name.text;
                    if (key === "Date.now")
                        flag(node, "no-wall-clock", "Date.now()", clock);
                    else if (key === "performance.now")
                        flag(node, "no-wall-clock", "performance.now()", clock);
                    else if (key === "Math.random")
                        flag(node, "no-unseeded-random", "Math.random()",
                            "randomness comes seeded from the engine bindings");
                }
            } else if (ts.isNewExpression(node) && ts.isIdentifier(node.expression) &&
                       node.expression.text === "Date") {
                flag(node, "no-wall-clock", "new Date()", clock);
            } else if (ts.isCallExpression(node)) {
                const callee = unGlobal(node.expression);
                if (ts.isIdentifier(callee)) {
                    if (callee.text === "setTimeout" || callee.text === "setInterval")
                        flag(node, "no-timer", callee.text + "()",
                            "scheduling is the tick loop's job (statecharts + sequences)");
                    else if (callee.text === "Date")
                        flag(node, "no-wall-clock", "Date()", clock);
                }
            }
            ts.forEachChild(node, visit);
        };
        visit(sf);
    };

    globalThis.__midday_ts_run = (request) => {
        const converted = ts.convertCompilerOptionsFromJson(request.options, "");
        if (converted.errors.length > 0)
            return { failure: "compiler options rejected: " +
                ts.flattenDiagnosticMessageText(converted.errors[0].messageText, " ") };
        const host = createHost(converted.options, request.lib_dir);
        if (read(request.entry) === null)
            return { failure: "cannot read entry " + request.entry };
        const program = ts.createProgram(
            [request.entry, request.engine_dts], converted.options, host);
        const diagnostics = ts.getPreEmitDiagnostics(program).map(typeDiagnostic);
        for (const sf of program.getSourceFiles())
            if (!sf.isDeclarationFile && !sf.fileName.startsWith(request.lib_dir + "/"))
                lintSourceFile(sf, (d) => diagnostics.push(d));
        const response = { diagnostics };
        if (request.emit && diagnostics.length === 0) {
            let js = null;
            const entrySource = program.getSourceFile(request.entry);
            const emitted = program.emit(entrySource, (name, text) => {
                if (name.endsWith(".js"))
                    js = text;
            });
            if (emitted.diagnostics.length > 0)
                return { failure: "emit failed: " + ts.flattenDiagnosticMessageText(
                    emitted.diagnostics[0].messageText, " ") };
            if (js === null)
                return { failure: "emit produced no .js for " + request.entry };
            response.js = js;
        }
        return response;
    };
})();
