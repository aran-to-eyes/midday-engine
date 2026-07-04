# ts/runtime

The embedded QuickJS script runtime (m0-quickjs-ts-toolchain) plus the
batch-first binding layer (m0-batch-bindings). `ScriptRuntime` owns the
JSRuntime/JSContext pair, its limits, the resolver-only module loader, and
the JSON host-hook seam; QuickJS types never leak past `script_runtime.cpp`
and `batch_views.cpp` (the one sanctioned typed-array exception, through the
`script::detail` seam).

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
- **Counters**: `alloc_bytes()` (cumulative JS-heap bytes allocated — a
  counting allocator behind `JS_NewRuntime2`; churn, not residency) and
  `host_calls()` (JSON-seam crossings). The batch budget instrumentation.
- **Batch views** (`batch_views.h`, the high-bandwidth boundary): scripts
  request `{components, fields}` ONCE via `midday/batch`; the engine
  publishes SoA typed-array views per phase (staged gather of the ACTIVE
  join — see the header's zero-copy-vs-staged decision record, D-BUILD-070),
  scripts compute over the arrays, and `commit()` scatters writable columns
  back at one deterministic point. Crossings per tick are O(exposed
  buffers), never O(entities); capacity growth DETACHES old ArrayBuffers
  (stale JS refs go dead deterministically); rows that vanish mid-phase
  refuse the commit (`bindings.stale_view`). Columns register at boot via
  `expose<T>().field<&T::m>()`, validated against the reflect ClassDesc —
  the generated `bindings_spec.json` `batch_envelope` is the spec they
  implement.
- **Bench harness** (`batch_bench.h`, `midday script bench`): the budget
  JSON behind the m0 exit tests — 1k/10k/100k sweep, crossings constant at
  16 for the 3-pool fixture (<= 8 * pool_count), zero steady-state GC bytes,
  naive per-field mode >= 10x chattier (measured: ~1100x at 1k).
- **State-script binding** (`state_script.h`, m0-appendix-a-determinism):
  authored `script:` modules seated on statechart states. `StateScriptHost`
  implements `statechart::StateHooks`; modules build through the toolchain
  cache, a generated per-seat shim registers each default-export class with
  the JS-side seat registry (instantiated at bind — boot-deterministic),
  and only the hooks a class actually has ever cross the boundary. Inside a
  hook, `__midday_emit(event, payload, key)` triggers on the bus with the
  hook's own journal record as the cause id (the A.3 cause-chain contract);
  symbolic keys resolve through an injected resolver (loader::resolve_key in
  production). The seam's global names are generated data —
  `api/bindings_spec.json` `state_script_hooks`, drift-gated by
  `golden.ts_hook_parity`. Hook faults surface via `first_error()`
  (tick-annotated); the run host fails the run loudly.
