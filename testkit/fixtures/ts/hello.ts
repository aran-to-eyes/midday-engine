// ts_hello — the canonical first script: typechecks against the ambient
// engine surface, transpiles on the vendored compiler, runs on the embedded
// QuickJS, and reports a known value through a minimal host hook.
declare function __midday_emit(value: number): void;

const answer: number = 6 * 7;
__midday_emit(answer);

export {};
