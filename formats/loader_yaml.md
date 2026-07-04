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
reflect vocabulary at spawn — the bus validates every trigger against them.

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
