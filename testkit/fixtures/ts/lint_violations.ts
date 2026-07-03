// Every door the engine lint pack (midday-lint/1) must slam, one per line.
// The declares make the file TYPE-clean on the es2020 lib: the lints fire on
// the AST regardless of whether the banned names typecheck.
declare const performance: { now(): number };
declare function setTimeout(callback: () => void, ms: number): number;
declare function setInterval(callback: () => void, ms: number): number;

const t0: number = Date.now();
const t1: number = performance.now();
const roll: number = Math.random();
const stamp: Date = new Date();
const timer: number = setTimeout(() => undefined, 16);
const ticker: number = setInterval(() => undefined, 16);

export { t0, t1, roll, stamp, timer, ticker };
