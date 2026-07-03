# core/statechart

The statechart entity runtime (m0-statechart-core + m0-sequences, spec §4.1
+ Appendix A — normative): every entity is a state machine by default.
Machines instantiate as subtrees of real entities (root under host, regions
under root, states under regions/parent states), so **state active IS
subtree active** — one mechanism, `core/hierarchy` activation underneath,
behavior visible in the tree. Sequences live HERE, not in a separate
library: a dope sheet is a state's body (§4.1), its spans close inside the
A.2.1 exit chain, and its playhead is per-state runtime data (D-BUILD-057).

Proven **C++-first**: the whole A.2/A.2.1 semantics fixture suite runs with
no QuickJS in the process. TS hook parity is a separate claim
(m0-appendix-a-determinism).

| file | contents |
|------|----------|
| `machine_desc.h` | `MachineDesc`/`RegionDesc`/`StateDesc`/`TransitionDesc`/`WatcherDesc`/`VarDesc`/`SequenceDesc` (trigger + span tracks, end modes) — plain, Name-referenced aggregates, exactly what YAML machine files parse into (m0-yaml-loader-run) |
| `statechart.h` | the runtime: `Statechart`, `StateHooks` (onEnter/onExit/onUpdate/onFixedUpdate), `MachineRoot`/`StateNode` components, `time_to_tick` (THE rounding rule), journal record inventory, allocation contract |
| `instance.h` | internal compiled form: flat slice-addressed state/transition/watcher/sheet tables + the playhead model |
| `instantiate.cpp` | atomic validate → compile (core/expr) → build subtree → deactivate non-initial → journal → subscribe → initial enter chains |
| `transitions.cpp` | the A.2 algorithm inline on bus events + recursive A.2.1 exit/enter chains + voiding |
| `watchers.cpp` | A.1 phase 3: `when:` watchers, tree order, edge semantics |
| `sequences.cpp` | A.1 phase 4: dope sheets — tick-locking compile, playhead advance, span open/close, loop/hold/finish, interruption + history (the A.2.1 sequence steps) |
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
- `<state>.finished` emission mechanism (`finish_state`) — sequences call it
  when a playhead completes; chaining is an ordinary pair on the event
  (`then:` sugar canonicalizes to that pair at the loader).
- Sequences: keyframe seconds tick-lock at instantiate
  (`llround(seconds * rate)`, ties away from zero — KAT-pinned); local tick L
  fires at global tick E + L (E = entry tick); items fire in timeline order
  (triggers, span openings, span closings); interruption closes open spans
  inside the exit chain (after the script's onExit, before the substate
  exits, reverse open order) and resets — or saves and resumes, with
  covering spans re-opening inside the enter chain — under `history: true`.
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

`midday selftest --filter sequence.*` — 8 cases: the rounding-rule KATs,
THE canonical fixture (1.2 s at 60 Hz: trigger at EXACTLY tick 18, span
open 24 / close 48, finished 72 — journal-walked with cause chains),
loop[2] wrap semantics (local 0 fires at entry AND at the wrap tick),
hold, mid-span interruption ordering pinned inside the exit chain, the
history resume variant (re-open inside the enter chain, early finish),
atomic validation refusals, and a dual-run byte-compare of an
interruption/loop scenario.
