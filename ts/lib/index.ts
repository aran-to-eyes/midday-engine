// midday — the bare module specifier (distinct from "midday/<name>",
// D-BUILD-072). The compiler-options paths mapping (ts/toolchain/
// toolchain.cpp) resolves `import ... from 'midday'` to exactly this file
// at BOTH typecheck and runtime, so there is one real source of truth for
// the component-authoring surface; engine.d.ts's ambient module block only
// AUGMENTS it with the generated per-event bare payload-type aliases
// (api/CODEGEN.md "Script component API").
export * from "./component";
