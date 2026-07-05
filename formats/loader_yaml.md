# Loader YAML — the M0 strict subset + m1-scene-format's in-place extension

The PERMANENT loader (`core/loader/`) reads scene / machine / events /
entity-prefab text (spec §8). `m1-scene-format` extends the M0 grammar
documented below **in place** — one loader, ever; every M0 fixture in this
repo keeps loading byte-for-byte unchanged (the node's exit-test #4). The
M1 additions are marked `(m1)` throughout; see "m1-scene-format additions"
at the bottom for the parts that don't fit inline (prefab/entity files,
override resolution, the strict/lenient boundary, `midday scene print`).

Common rules (strictness is the product):

- Every file carries `format: 1`; unknown/absent versions refuse loudly.
- STRICT YAML: no anchors, no aliases, no tags, no duplicate keys, one
  document per file. No implicit typing — quoted scalars are always
  strings; numbers use the core JSON number grammar.
- Unknown keys, bad references (goto/initial/event/key/path), type
  mismatches, and duplicate names refuse with a structured error carrying
  `file:line:col` (exit 3 at the CLI).
- All asset/script paths are **project-root-relative**; in M0 the project
  root is the scene file's directory.

## `*.events.yaml`

```yaml
format: 1
events:
  death.dealt: {payload: {by: entity_ref}, doc: lethal damage landed}
  attack.swoosh: {payload: {}}
keys: [squad]            # group channels usable as `key:` (besides self/root/global)
```

Payload types are canonical reflect `TypeDesc` spellings (`float`, `int`,
`bool`, `string`, `name`, `vec2/3/4`, `quat`, `color`, `entity_ref`,
`asset_ref`, `array<T>`, `map<T>`). Declared events register into the
reflect vocabulary at spawn — the bus validates every trigger against them
(a wrong-typed trigger halts the run with a structured, tick-annotated
error — `core/bus/bus.cpp` `bus.payload_invalid`).

**Project-wide namespace + collision checks** (m1-events-format): event
names live in ONE namespace across every `*.events.yaml` a project owns,
not just the files one scene happens to list. `midday validate
<file>.events.yaml` (no `--schema`/`--schema-file`; extension dispatch)
proves this: it walks `<file>`'s own directory recursively for every
`*.events.yaml` under it (the project root, until `m1-project-new` defines
a real one — the same "project root = the file's directory" convention
scenes already use) and merges them into one vocabulary via repeated
`load_events_file` calls — the SAME merge that already makes a scene's own
`events: [...]` list cross-file-duplicate-safe, just spanning the whole
root. A name declared twice under that root refuses `loader.duplicate` at
the second file's `file:line:col`, exit 3 — even when no single scene
lists both files. `core/loader/loader.h`'s `load_project_events` is the
mechanism; `midday run` still loads each scene's explicitly-listed events
files unchanged (this layer only widens VALIDATION's view, not runtime
scene loading).

## `*.input.yaml` / `*.input_profile.yaml` (m1-input-actions)

```yaml
format: 1
actions:
  jump:
    bindings:
      - {device: keyboard, control: space}
      - {device: gamepad, control: button_south}
  move_up: {bindings: [{device: keyboard, control: w}]}
  move_down: {bindings: [{device: keyboard, control: s}]}
  move_left: {bindings: [{device: keyboard, control: a}]}
  move_right: {bindings: [{device: keyboard, control: d}]}
sticks:
  move:                     # a named 2D composite (Godot InputMap::get_vector precedent)
    up: move_up             # up/down/left/right must already be declared actions
    down: move_down
    left: move_left
    right: move_right
    deadzone: 0.2           # optional, default 0.2; valid range [0, 1)
```

Device kinds are a closed vocabulary (`keyboard`, `mouse`, `gamepad`,
`touch`); `control` is an opaque device-specific string — a real OS/device
backend is `m7-platform` territory, so this format is DATA plus the
synthetic injection seam (`core/input/inject.h`), never a hardware mapping.
Action names are a **project-wide namespace**, exactly like events:
`midday validate <f>.input.yaml` (extension dispatch, no `--schema` flag)
walks `<f>`'s own directory recursively for every `*.input.yaml` under it
and merges them (`core/loader/loader.h` `load_project_input`) — two
DIFFERENT actions anywhere under that root binding the same
`(device, control)` refuse `input.conflict`, exit 3 (spec section 13:
"conflict detection in the validator").

**Runtime rebinding overlay** (spec section 13: "a user-profile overlay"):

```yaml
format: 1
overlay:
  jump:
    bindings:
      - {device: keyboard, control: enter}
```

`*.input_profile.yaml` loads as a single file (no project merge — a
player's profile is not a project namespace) with the SAME action/binding
grammar under `overlay:` instead of `actions:` (no `sticks:`: a rebind
changes which control drives an action, never which actions feed a stick).
`apply_overlay()` combines a loaded profile against a base `ActionMapDecl`:
every action the overlay mentions has its bindings REPLACED wholesale, then
the merged result is re-validated for conflicts (a rebind can introduce a
collision the original authoring never had).

## `*.machine.yaml`

```yaml
format: 1
machine: boss
vars: {health.current: float}         # THE expression environment (if: filters)
regions:
  combat:
    initial: Passive
    anystate:                          # region-owned any-state rules
      - {event: death.dealt, key: self, goto: Dead, priority: 100}
    states:
      Passive:
        on:                            # sugar -> Transition pairs, declaration order
          - {event: player.inRange, goto: SlashAttack, if: 'health.current > 0'}
      Chase:                           # (m1) a state's own component set
        components:                    # (m1) — spec 4.1 "states owning component sets"
          - NavFollow: {speed: 4.5, repathEvery: 0.25}
        Transition:                    # (m1) `on:`'s CANONICAL, already-desugared
          - {event: player.lost, goto: Passive, priority: 0}   # spelling — never author both
      SlashAttack:
        script: scripts/slash_attack.ts
        initial: Windup                # required iff `states:` present
        history: false
        sequence:
          duration: 1.2               # seconds; tick-locked at instantiate
          end: finish                  # finish | loop | hold
          loop: 2                      # end: loop only; total passes, 0 = forever
          then: Passive                # sugar -> {<state>.finished -> Passive} pair
          tracks:
            - trigger: [{t: 0.30, event: attack.swoosh, payload: {}, key: self}]  # key: (m1)
            - span: {name: HitboxLive, from: 0.40, to: 0.80}   # `state:` is name:'s (m1) alias
        on:
          - {event: self.finished, goto: Passive}   # sugar -> <state>.finished
        states:
          Windup: {}
          HitboxLive:
            children:                  # child entities under the STATE node
              - entity: Hurtbox
                at: [0, 1.0, 1.2]
                components:            # (m1) a child's own components, generic
                  - Collider: {shape: box, size: [0.6, 0.6, 1.4], trigger: true}
                  - DamageOnTouch: {amount: 40, stagger: 8}
```

Pair `key:` vocabulary: `self` / `root` (the host's private channel —
always subscribed), `global`, or a group declared in an events file's
`keys:`; global/group keys join `MachineDesc::channels`. Event names in
pairs/triggers must be declared, built-in, or derived
(`<state>.finished`, `<span>.opened` / `.closed`). `if:` filters compile
against `vars:` at load. Goto targets resolve region-wide by name.

**`on:` / `Transition:` (m1, spec 4.2 SPEC-GAP #5)**: `on:` is authoring
sugar for a `Transition` component pair list; `Transition:` is that exact,
already-desugared spelling — both parse into the identical
`StateDesc::transitions`, so a state may author EITHER but never both.
`midday scene print --full` on a `*.machine.yaml` file always emits
`Transition:` (never `on:`), with `priority:`/`history:` filled to their
defaults explicitly (spec §369 "full-state visibility") — round-trip
stable: reloading that printed text reproduces the identical `MachineDesc`
(exit-test #1). A pair's `key:` is NOT retained in the canonical form
(event names are matched across every subscribed channel regardless,
D-BUILD-046) — a documented, deliberate simplification.

**A state's own `components:` (m1, spec 4.1 "states owning component
sets")**: a state (or a state child under `children:`) may carry a
`components:` list, the SAME `{Name: {field: value, ...}}` shape a scene
entity's `components:` uses. The vocabulary is open-ended (native
Transform/Collider/RigidBody plus, optionally, names extracted from a
project's TS component schemas, `midday script extract`) — an unknown name
is a hard refusal by default and a reported Gap only when the caller
opts into lenient mode (`midday scene print`; see below). Pure data in
this milestone: no component type is actually instantiated/activated by
the statechart runtime yet.

**Sequence trigger `key:` (m1, "symbolic keys")**: a trigger keyframe may
carry the same `self`/`root`/`global`/`<group>` vocabulary a transition
pair's `key:` uses. `self`/`root` already match the runtime's existing
"always the host channel" trigger behavior (`core/statechart/
sequences.cpp`) exactly; routing an emit to `global`/a group is deferred
(not this node's runtime to extend) — the spelling is grammar-accepted and
validated, never wired to real cross-channel routing yet.

**Span `state:` (m1 alias)**: `span: {state: <Name>, from, to}` is an
accepted alternative to `span: {name: <Name>, from, to}` (a span's name
always equals the substate it opens, D-BUILD-081) — carrying both refuses.
Canonical re-emission always spells it `name:`.

**A state with substates but no `initial:` (m1, lenient-only)**: normally a
hard refusal (a region/state must know where to enter). A state whose ONLY
substates are span targets (entered/exited natively by the span's own
open/close, never a normal `initial:` descent) is legitimate future
content — `m4` binds span-scoped activation natively; until then, lenient
mode reports a Gap and picks the first substate as a syntactically-valid
placeholder so the rest of the tree still parses; every other caller keeps
the exact M0 hard refusal.

## `*.scene.yaml`

```yaml
format: 1
scene: boss_arena
events: [boss.events.yaml]             # the project vocabulary, loaded first
entities:
  - entity: Ground
    components:
      - Transform: {}                  # {at: [x, y, z]} — local TRS translation
      - Collider: {shape: plane}       # static ground plane at the transform's y
  - entity: Boss
    components:
      - Transform: {at: [0, 1.0, 0]}
      - Collider: {shape: box, size: [1.2, 1.8, 1.2]}   # full extents
      - RigidBody: {}                  # dynamic; box+RigidBody = the M0 dynamic body
      - MeshRenderer: {model: {uid: "m:abc123", path: models/boss.model.yaml}}  # (m1) generic
    machines:
      - {instance: {uid: "m:def456", path: boss.machine.yaml}}   # (m1) uid dual-write
  - entity: BossPrefabInstance          # (m1) a PREFAB entity, mutually exclusive
    prefab: {uid: "m:ghi789", path: prefabs/boss.entity.yaml}    # with components:/machines:
    at: [6, 0, 0]                                                # `at:` sugar (position only)
    override:                          # property-diff, path-addressed BY NAME (never index)
      boss/combat/SlashAttack/sequence: {duration: 1.0}          # <machine-name>/<Region>/...
```

The M0 native component vocabulary is exactly the runtime that exists:
`Transform` (hierarchy local TRS), `Collider` + `RigidBody` (the M0 physics
surface) — typed, strict, unchanged since M0. A box collider without a
RigidBody (static boxes: m4-physics-full) and a RigidBody without a box
collider still refuse unconditionally. **Every OTHER component name is
generic (m1)** — `{Name: {...}}` verbatim, checked against the same
open-ended vocabulary machine state components use; unknown is a hard
refusal by default, a Gap when lenient.

**`prefab:` + `at:` + `override:` (m1, spec 4.1 "machines are prefab
subtrees ... instanced as assets with per-entity property-diff
overrides")**: an entity is either INLINE (`components:`/`machines:`) or a
PREFAB INSTANCE (`prefab:`, optionally `at:`/`override:`) — never both. A
missing prefab file is a lenient-only Gap (brand-new grammar, no M0
precedent to stay strict about); `at:` sets the effective Transform
translation; `override:` property-diffs into the prefab's own machine
instance(s) — see "Override path grammar" below.

## `*.entity.yaml` (m1) — prefab/entity files

```yaml
format: 1
entity: Warden
base:                                  # EVERY entry generic (even Transform/Collider/
  - Transform: {}                      # RigidBody) — typed re-derivation is m1-prefab-spawn's
  - Collider: {shape: capsule, r: 0.6, h: 2.2, layer: enemy}
  - Health: {max: 120}
machines:
  - instance: {uid: "m:brn7q2", path: brains/warden.machine.yaml}  # ALWAYS hard-required
    override:                          # scoped to this ONE instance — no machine-name hop
      combat/SlashAttack/HitboxLive/Hurtbox/DamageOnTouch: {amount: 55}
attachments:                           # sockets: an asset, optionally a nested entity
  - socket: grip
    of: {uid: "m:mace09", path: models/warden_mace.model.yaml}     # lenient-only Gap if missing
    entity: {prefab: prefabs/warden_mace.entity.yaml}               # a bare path (no uid slot)
```

`base:` is entirely generic (open-ended vocabulary, same rules as a scene
entity's non-native components) — turning it back into typed native data
is `m1-prefab-spawn`'s job. A `machines[].instance` file is ALWAYS
hard-required (a machine file is structurally necessary to describe
anything at all) in every mode; `attachments[]` asset refs (`of:`, the
nested `entity: {prefab: ...}`) are lenient-only Gaps — brand-new grammar
with no M0 precedent to preserve.

## Override path grammar (m1, spec 4.2)

`<machine-name>/<Region>/<State>/.../{sequence | <Component> |
<ChildEntity>/<Component>}` — resolved BY NAME at every step, never by
index (`core/loader/override.h`): the region is looked up by name; each
subsequent segment is matched against the CURRENT container's direct
children (the region's top-level states, then that state's own substates,
and so on) — reordering or adding sibling states/children anywhere else in
the tree can never change what a path resolves to. The leaf is either
`sequence` (a property diff onto that state's dope sheet — `duration`,
`end`, `loop`; tracks/`then` are structural, never diffable), a component
the state owns directly, or `<ChildEntity>/<Component>` for a component
under one of the state's `children:`. The `<machine-name>` segment is
required ONLY at the scene level (`prefab: ... override:`, since an entity
may own more than one machine instance); an entity/prefab file's own
`machines[].override:` is already scoped to one instance and omits it. A
bad path (unknown region/state/child/component, a `sequence` target with
none) is ALWAYS a hard refusal — an override addresses something by name,
so a wrong name is an authoring bug, never a "not implemented yet" Gap.

## m1-scene-format additions — the strict/lenient boundary

Every M0 refusal stays a hard refusal in EVERY mode (default). A NEW,
opt-in LENIENT mode (`core/loader/gaps.h`; `load_scene` / `load_machine_file`
/ `load_entity_file`'s trailing `lenient` argument, default `false`
everywhere) downgrades exactly the categories that are legitimately
"content this engine does not implement yet" into a collected `Gap`
instead of a refusal: an unknown component TYPE name, a missing state
script file, a missing prefab/model/attachment asset file, an event name
not visible from a standalone machine file's own directory, and the
"substates but no initial" native-span-activation gap above. Grammar
mistakes (unknown YAML keys, bad override paths, duplicate names, bad
types) are NEVER downgraded, in any mode. The only lenient caller today is
`midday scene print` — the honest-inspection verb (exit-test #5): it loads
a scene/machine/entity file, always leniently, and reports every gap
found anywhere in the file (and everything its resolved prefabs/machines
reference too) instead of either crashing or pretending completion.
`midday run` / `midday validate` / `midday fmt` stay fully strict, always.

`midday scene print <file> [--full] [--components <manifest>]`
(`cli/verbs/scene.cpp`) dispatches by extension
(`.scene.yaml`/`.machine.yaml`/`.entity.yaml`). Default: the canonical
strict-YAML text (`midday fmt`'s own mechanism). `--full`: for a machine
file, the canonical `MachineDesc` re-serialized (`core/loader/
machine_emit.h`, on:->Transition, defaults filled, round-trip stable); for
a scene/entity file, the raw canonical text plus a per-machine-instance
`overrides` report (each override path resolved and, when clean, the
resulting machine's OWN canonical form — `effective_yaml` — so an
overridden field is visible verbatim).

`api/schema_manifest.json`'s `formats[]` (m1-scene-format's reserved slot,
`ts/codegen/manifest.ts`) carries scene/machine/entity format-entry schemas
`midday validate --schema <name>` runs through the SAME generic engine
every other format uses (`core/loader/format_schema.h`) — extended with a
"modest" nested-shape mechanism (`kind: object` / `array_of_object` fields,
possibly opaque) sufficient to check a document's gross top-level SHAPE.
This validates a NECESSARY but not SUFFICIENT condition: the loader above
(`load_scene`/`load_machine_file`/`load_entity_file`) stays the real
semantic authority (event vocab, region-wide name resolution, override-path
resolution, uid resolution) — `midday validate` is a shape sanity check,
never a substitute for actually loading the file.

Implementation: `core/loader/` (yaml wrapper, format loaders, override
engine, generic component/asset-ref parsing, spawn). Decisions:
D-BUILD-076..081 (M0), this node's report (M1). Consumers: `midday run`,
`midday scene print`, `midday validate`, `midday journal diff`.
