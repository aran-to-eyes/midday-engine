# core/statechart

The statechart entity runtime (m0-statechart-core, spec §4.1 + Appendix A —
normative): every entity is a state machine by default. Machines instantiate
as subtrees of real entities (root under host, regions under root, states
under regions/parent states), so **state active IS subtree active** — one
mechanism, `core/hierarchy` activation underneath, behavior visible in the
tree.

Proven **C++-first**: the whole A.2/A.2.1 semantics fixture suite runs with
no QuickJS in the process. TS hook parity is a separate claim
(m0-appendix-a-determinism).

| file | contents |
|------|----------|
| `machine_desc.h` | `MachineDesc`/`RegionDesc`/`StateDesc`/`TransitionDesc`/`WatcherDesc`/`VarDesc` — plain, Name-referenced aggregates, exactly what YAML machine files parse into (m0-yaml-loader-run) |
| `statechart.h` | the runtime: `Statechart`, `StateHooks` (onEnter/onExit/onUpdate/onFixedUpdate), `MachineRoot`/`StateNode` components, journal record inventory, allocation contract |
| `instance.h` | internal compiled form: flat slice-addressed state/transition/watcher tables |
| `instantiate.cpp` | atomic validate → compile (core/expr) → build subtree → deactivate non-initial → journal → subscribe → initial enter chains |
| `transitions.cpp` | the A.2 algorithm inline on bus events + recursive A.2.1 exit/enter chains + voiding |
| `watchers.cpp` | A.1 phase 3: `when:` watchers, tree order, edge semantics |
| `hooks.cpp` | phase 5 onFixedUpdate driving + frame-side `run_update()` |

Semantics quick map (details in `statechart.h`'s header comment):
- transitions run **inline** on events; one subscription per key, tables
  filter names (D-BUILD-046); one transition per region per **tick** (stamp,
  D-BUILD-054); losers and later matching pairs journal as `statechart.voided`
  with reasons; cascades share the bus depth cap.
- A.2.1 recursively: exit scripts outer→inner (brain first), deactivation
  completes deepest-first (no zombie hitbox); enter is the exact mirror.
- `when:` watchers: armed false at entry, fire on observed true, re-arm on
  observed false or exit; fired with the phase marker as cause.
- `<state>.finished` emission mechanism (`finish_state`) — the m0-sequences
  attachment point; chaining is an ordinary pair on the event.
- Expression environment: the machine's declared `vars` only, bound via
  `set_var` (loader binds component fields later; is-in-state predicates
  arrive as loader-bound vars — no entity_ref value kind in core/expr).

Exit tests: `midday selftest --filter statechart.*` — 18 cases covering
priority, declaration-order tie, any-state ordering, region marking +
region-wide voiding, cascade depth cap + own-region cycle breaker, voided
record shape (byte-pinned payload), watcher edge semantics + faults, the
pinned A.2.1 nested hook sequence with hurtbox dormancy probes, history
vs initial re-entry, state.finished chaining, and a dual-run journal
byte-compare of a scripted scenario.
