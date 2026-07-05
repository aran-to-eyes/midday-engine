# Loader YAML — the M0 strict subset (scene / machine / events)

The PERMANENT loader (`core/loader/`, m0-yaml-loader-run) reads three text
formats (spec §8). `m1-scene-format` extends this grammar **in place**
(schema-manifest validation, uid dual-write, prefab overrides) — there is
never a second loader, and every M0 file keeps loading unchanged.

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
            - trigger: [{t: 0.30, event: attack.swoosh, payload: {}}]
            - span: {name: HitboxLive, from: 0.40, to: 0.80}
        on:
          - {event: self.finished, goto: Passive}   # sugar -> <state>.finished
        states:
          Windup: {}
          HitboxLive:
            children:                  # child entities under the STATE node
              - {entity: Hurtbox, at: [0, 1.0, 1.2]}
```

Pair `key:` vocabulary: `self` / `root` (the host's private channel —
always subscribed), `global`, or a group declared in an events file's
`keys:`; global/group keys join `MachineDesc::channels`. Event names in
pairs/triggers must be declared, built-in, or derived
(`<state>.finished`, `<span>.opened` / `.closed`). `if:` filters compile
against `vars:` at load. Goto targets resolve region-wide by name.

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
    machines:
      - {instance: {path: boss.machine.yaml}}   # uid dual-write joins at m1
```

The M0 component vocabulary is exactly the runtime that exists: `Transform`
(hierarchy local TRS), `Collider` + `RigidBody` (the M0 physics surface).
A box collider without a RigidBody (static boxes: m4-physics-full), a
RigidBody without a box collider, and unknown components all refuse.

Implementation: `core/loader/` (yaml wrapper, three format loaders, spawn).
Decisions: D-BUILD-076..081. Consumers: `midday run`, `midday journal diff`.
