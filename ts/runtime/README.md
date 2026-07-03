# ts/runtime

The embedded QuickJS script runtime (m0-quickjs-ts-toolchain). `ScriptRuntime`
owns the JSRuntime/JSContext pair, its limits, the resolver-only module
loader, and the JSON host-hook seam; QuickJS types never leak past
`script_runtime.cpp`.

- **Two profiles**: SIM (default — Date / performance / Math.random poisoned
  by a JS prelude, throwing `script.nondeterminism`; no timers exist because
  quickjs-libc is not vendored) and TOOL (`deterministic = false`, used by
  ts/toolchain for the vendored compiler). No unpoison API — bypass policy:
  none.
- **Gas**: the interrupt budget counts handler invocations (one per 10000 VM
  poll points — calls + loop back-edges), a pure function of the executed
  program. Deterministic, never wall-clock.
- **Errors**: JS exceptions become `base::Error` — codes `script.exception` /
  `script.syntax` / `script.out_of_memory` / `script.interrupted`, details
  `{file, line, col, stack}`; the sim caller fills the `{tick,
  replay_bookmark}` slots via `annotate_sim_context`.
- Batch bindings (the high-bandwidth boundary) arrive at m0-batch-bindings;
  the JSON host hooks here are the bootstrap seam, not the fast path.
