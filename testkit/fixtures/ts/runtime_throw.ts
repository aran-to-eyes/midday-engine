// Typechecks and lints clean, then throws during module evaluation: the
// runtime must surface a structured error with stack + file:line and empty
// {tick, replay_bookmark} slots for the sim caller to fill.
export function detonate(): never {
    throw new Error("fixture detonation");
}

detonate();
