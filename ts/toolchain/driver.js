// ts/toolchain/driver.js — first-party compiler driver, evaluated on the
// embedded QuickJS right after the vendored typescript.js (global `ts`).
// One entry point: globalThis.__midday_ts_run(request) -> response.
//
//   request:  { entry, engine_dts, lib_dir, options, emit, extract }
//   response: { diagnostics: [{kind, code, file, line, col, message}...],
//               js?, components?, failure? }  // failure = infrastructure,
//                                              // never a code problem
//
// `extract` (m1-ts-components): when true and diagnostics come back empty,
// walk the ENTRY file's AST for @component()-decorated classes and set
// `components` — see extractComponents below. A schema-extraction problem
// pushes a "schema"-kind diagnostic into the SAME array instead of setting
// `components`, so it fails clean exactly like a type error or lint hit.
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

    // Component schema extraction (m1-ts-components): a TypeScript AST walk
    // over the ENTRY file only — the same "never regex over text" mechanism
    // as the lint pack above — that reads @component()/@field()-decorated
    // classes STRAIGHT FROM THE SYNTAX. The code is never run: decorator
    // detection is by IDENTIFIER TEXT (never the checker), field/param/
    // return types come from an explicit type annotation — bare names from
    // the field-type table; 'midday'-qualified spellings additionally gated
    // on what engine.d.ts declares (see typeFromAnnotation) — or (fields
    // only, absent an annotation) the initializer's literal kind, and every
    // "default"/@field(...) argument value is read as a literal (numeric,
    // string, boolean, or an array of those) — nothing is evaluated.
    // Unrecognized shapes push a "schema" diagnostic into the SAME array the
    // type/lint passes use, so an unextractable field fails validate-before-
    // write exactly like a type error or lint hit (CheckOutcome.ok gates on
    // one array). Called only when request.extract && diagnostics.length
    // === 0 (extraction never runs against an already-dirty compile).
    const TYPE_KEYWORD_KIND = {
        [ts.SyntaxKind.NumberKeyword]: "float",
        [ts.SyntaxKind.StringKeyword]: "string",
        [ts.SyntaxKind.BooleanKeyword]: "bool",
    };
    const TYPE_REFERENCE_NAME = {
        EntityRef: "entity_ref", Vec2: "vec2", Vec3: "vec3", Vec4: "vec4",
        Quat: "quat", Color: "color",
    };

    // The rightmost identifier of a 'midday'-qualified annotation —
    // `import('midday').X` (ImportTypeNode) or `midday.X` (QualifiedName
    // TypeReference) — or null for any other shape. Only the literal
    // 'midday' qualifier is accepted; `somethingElse.X` stays unknown.
    // Matched by NAME TEXT, never the checker: a local `namespace midday`
    // shadowing the ambient one is the same accepted, documented gap as
    // decoratorCall's identifier-text matching below.
    const middayQualifiedName = (node) => {
        if (ts.isImportTypeNode(node)) {
            if (node.argument === undefined || !ts.isLiteralTypeNode(node.argument) ||
                !ts.isStringLiteral(node.argument.literal) ||
                node.argument.literal.text !== "midday") return null;
            return node.qualifier !== undefined && ts.isIdentifier(node.qualifier)
                ? node.qualifier.text : null;
        }
        if (ts.isTypeReferenceNode(node) && ts.isQualifiedName(node.typeName) &&
            ts.isIdentifier(node.typeName.left) && node.typeName.left.text === "midday" &&
            ts.isIdentifier(node.typeName.right))
            return node.typeName.right.text;
        return null;
    };

    // A written type annotation -> canonical reflect TypeDesc spelling, or
    // null when the annotation names something extraction does not know
    // (api/CODEGEN.md "Script component API" field-type table).
    // 'midday'-qualified spellings (m1-exit Phase 3, CONCERNS #12a) resolve
    // SYNTACTICALLY against the engine.d.ts the program already carries.
    // Layering (proven by the three negative fixtures): the CHECKER owns
    // existence, PER SURFACE — import('midday').X checks against the
    // ambient module's re-export list (value types + authoring surface
    // only, engine.d.ts:429), midday.X against the full namespace — so
    // Nonexistent and module-unexported names (TriggerEnteredEvent) die as
    // TS2694 before extraction ever runs; THIS gate owns the TypeDesc
    // mapping and doubles as drift-defense should a compiler change ever
    // demote unknown members to `any`. A qualified name
    // extracts only when the d.ts actually declares it AND the field-type
    // table maps it; d.ts membership without a table row
    // (TriggerEnteredEvent) and table rows the d.ts lost both stay null —
    // fail-closed, identical to a bare unknown TypeReference. The name set
    // is trusted because engine.d.ts is a GENERATED artifact the CI drift
    // lane byte-pins to codegen — a hand-divergent d.ts cannot land.
    const typeFromAnnotation = (node, engineTypeNames) => {
        if (TYPE_KEYWORD_KIND[node.kind] !== undefined) return TYPE_KEYWORD_KIND[node.kind];
        if (ts.isTypeReferenceNode(node) && ts.isIdentifier(node.typeName))
            return TYPE_REFERENCE_NAME[node.typeName.text] ?? null;
        const qualified = middayQualifiedName(node);
        if (qualified !== null)
            return engineTypeNames.has(qualified)
                ? (TYPE_REFERENCE_NAME[qualified] ?? null) : null;
        if (ts.isArrayTypeNode(node)) {
            const element = typeFromAnnotation(node.elementType, engineTypeNames);
            return element === null ? null : "array<" + element + ">";
        }
        return null;
    };

    // Top-level type names `declare namespace midday { ... }` actually
    // declares in engine.d.ts. TYPE_REFERENCE_NAME says what a name MEANS;
    // the d.ts says whether it EXISTS today — qualified-import extraction
    // gates on both so a codegen change can never be fabricated around.
    const collectEngineTypeNames = (dtsSource) => {
        const names = new Set();
        if (dtsSource === undefined) return names;
        const visit = (statements) => {
            for (const st of statements) {
                if (ts.isModuleDeclaration(st) && st.name !== undefined &&
                    st.name.text === "midday" && st.body !== undefined &&
                    ts.isModuleBlock(st.body))
                    visit(st.body.statements);
                else if ((ts.isInterfaceDeclaration(st) || ts.isTypeAliasDeclaration(st) ||
                          ts.isEnumDeclaration(st) || ts.isClassDeclaration(st)) &&
                         st.name !== undefined)
                    names.add(st.name.text);
            }
        };
        visit(dtsSource.statements);
        return names;
    };

    // A literal AST node -> its plain JS value, or undefined when the node
    // is not one of the literal kinds extraction understands (never a
    // computed/identifier/call expression — nothing here evaluates).
    const literalValue = (node) => {
        if (ts.isNumericLiteral(node)) return Number(node.text);
        if (node.kind === ts.SyntaxKind.PrefixUnaryExpression &&
            node.operator === ts.SyntaxKind.MinusToken && ts.isNumericLiteral(node.operand))
            return -Number(node.operand.text);
        if (ts.isStringLiteral(node) || ts.isNoSubstitutionTemplateLiteral(node)) return node.text;
        if (node.kind === ts.SyntaxKind.TrueKeyword) return true;
        if (node.kind === ts.SyntaxKind.FalseKeyword) return false;
        if (ts.isArrayLiteralExpression(node)) {
            const out = [];
            for (const element of node.elements) {
                const value = literalValue(element);
                if (value === undefined) return undefined;
                out.push(value);
            }
            return out;
        }
        return undefined;
    };

    // A literal initializer (no type annotation on the field) -> its
    // inferred TypeDesc spelling, or null when the literal shape is not one
    // extraction can type (e.g. an empty array — nothing to infer from).
    const typeFromLiteral = (node) => {
        if (ts.isNumericLiteral(node)) return "float";
        if (node.kind === ts.SyntaxKind.PrefixUnaryExpression &&
            node.operator === ts.SyntaxKind.MinusToken)
            return "float";
        if (ts.isStringLiteral(node) || ts.isNoSubstitutionTemplateLiteral(node)) return "string";
        if (node.kind === ts.SyntaxKind.TrueKeyword || node.kind === ts.SyntaxKind.FalseKeyword)
            return "bool";
        if (ts.isArrayLiteralExpression(node) && node.elements.length > 0) {
            const element = typeFromLiteral(node.elements[0]);
            return element === null ? null : "array<" + element + ">";
        }
        return null;
    };

    // The `@name(...)` decorator call on `node`, matched by IDENTIFIER TEXT
    // only (never the checker — a locally shadowed same-named decorator is
    // an accepted, documented gap). Null when absent.
    const decoratorCall = (node, name) => {
        if (!ts.canHaveDecorators(node)) return null;
        for (const decorator of ts.getDecorators(node) ?? [])
            if (ts.isCallExpression(decorator.expression) &&
                ts.isIdentifier(decorator.expression.expression) &&
                decorator.expression.expression.text === name)
                return decorator.expression;
        return null;
    };

    // The decorator call's first argument, an object literal of literal
    // values (e.g. `{min: 0}`) -> a plain key/value object. Undefined when
    // there is no argument (bare `@field()`); null when the shape is
    // anything extraction cannot statically read.
    const decoratorOptions = (call) => {
        if (call.arguments.length === 0) return undefined;
        const arg = call.arguments[0];
        if (!ts.isObjectLiteralExpression(arg)) return null;
        const out = {};
        for (const prop of arg.properties) {
            if (!ts.isPropertyAssignment(prop)) return null;
            const key = ts.isIdentifier(prop.name) ? prop.name.text
                : ts.isStringLiteral(prop.name) ? prop.name.text
                : null;
            const value = key === null ? undefined : literalValue(prop.initializer);
            if (key === null || value === undefined) return null;
            out[key] = value;
        }
        return out;
    };

    function extractComponents(sourceFile, pushDiagnostic, engineTypeNames) {
        const schemaError = (node, message) => {
            const pos = sourceFile.getLineAndCharacterOfPosition(node.getStart(sourceFile));
            pushDiagnostic({
                kind: "schema", code: "schema.unresolved_type", file: sourceFile.fileName,
                line: pos.line + 1, col: pos.character + 1, message,
            });
        };
        const components = [];
        for (const node of sourceFile.statements) {
            if (!ts.isClassDeclaration(node) || node.name === undefined) continue;
            const classCall = decoratorCall(node, "component");
            if (classCall === null) continue;
            const fields = [];
            const methods = [];
            let clean = true;
            for (const member of node.members) {
                if (ts.isPropertyDeclaration(member) && ts.isIdentifier(member.name)) {
                    const fieldCall = decoratorCall(member, "field");
                    if (fieldCall === null) continue;
                    const type = member.type !== undefined
                        ? typeFromAnnotation(member.type, engineTypeNames)
                        : member.initializer !== undefined ? typeFromLiteral(member.initializer)
                        : null;
                    if (type === null) {
                        schemaError(member, "@field " + member.name.text + ": cannot determine " +
                            "a schema type (add an explicit type annotation or a literal " +
                            "initializer) — api/CODEGEN.md \"Script component API\"");
                        clean = false;
                        continue;
                    }
                    const field = { name: member.name.text, type };
                    if (member.initializer !== undefined) {
                        const value = literalValue(member.initializer);
                        if (value !== undefined) field.default = value;
                    }
                    const options = decoratorOptions(fieldCall);
                    if (options === null) {
                        schemaError(fieldCall, "@field(...) on " + member.name.text + " must be " +
                            "a plain object literal of literal values");
                        clean = false;
                        continue;
                    }
                    if (options !== undefined) for (const key of Object.keys(options)) field[key] = options[key];
                    fields.push(field);
                } else if (ts.isMethodDeclaration(member) && ts.isIdentifier(member.name)) {
                    const params = [];
                    let paramsClean = true;
                    for (const param of member.parameters) {
                        const type = ts.isIdentifier(param.name) && param.type !== undefined
                            ? typeFromAnnotation(param.type, engineTypeNames) : null;
                        if (type === null) {
                            paramsClean = false;
                            break;
                        }
                        params.push({ name: param.name.text, type });
                    }
                    if (!paramsClean) {
                        schemaError(member, member.name.text + "(...): every parameter needs a " +
                            "recognized explicit type annotation");
                        clean = false;
                        continue;
                    }
                    const method = { name: member.name.text, params };
                    if (member.type !== undefined) {
                        const returns = typeFromAnnotation(member.type, engineTypeNames);
                        if (returns === null) {
                            schemaError(member.type, member.name.text + "(): unrecognized return type");
                            clean = false;
                            continue;
                        }
                        method.returns = returns;
                    }
                    methods.push(method);
                }
            }
            if (clean) components.push({ name: node.name.text, file: sourceFile.fileName, fields, methods });
        }
        return components;
    }

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
        if (request.extract && diagnostics.length === 0) {
            const entrySource = program.getSourceFile(request.entry);
            response.components = extractComponents(entrySource, (d) => diagnostics.push(d),
                collectEngineTypeNames(program.getSourceFile(request.engine_dts)));
            if (diagnostics.length > 0) delete response.components; // a schema error dirtied it
        }
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
