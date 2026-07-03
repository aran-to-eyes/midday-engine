// Proves game code typechecks against the GENERATED engine surface: the
// ambient `midday` namespace comes from api/engine.d.ts, which is in every
// toolchain program (and in the cache key — regenerating the API invalidates
// stale script builds).
const spawn_point: midday.Vec3 = { x: 0, y: 1.5, z: 0 };
const tint: midday.Color = { r: 1, g: 0.5, b: 0.25, a: 1 };

export { spawn_point, tint };
