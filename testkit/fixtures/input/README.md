# input-actions fixture corpus (m1-input-actions)

Self-contained `*.input.yaml` / `*.input_profile.yaml` documents (the
`widget.schema.json` / `uid` precedent: purpose-built fixtures, NOT a real
scene/machine — that's `m1-scene-format`'s job). Each scenario lives in its
OWN subdirectory: `midday validate <file>` walks `<file>`'s own parent
directory recursively for project-wide merging (`load_project_input`), so
subdirectories are the isolation boundary between scenarios, not just
organization.

- `clean/` — two DIFFERENT files (`a.input.yaml`, `b.input.yaml`) under the
  SAME root: proves the action namespace is project-wide, not just the one
  file named on the command line (mirrors `m1-events-format`'s a.one/b.two
  split). `a.input.yaml` also declares the `move` virtual stick over its own
  four directional actions.
- `conflict/` — the EXIT-TEST #3 fixture: `a.input.yaml`'s `jump` and
  `b.input.yaml`'s `crouch` both bind `keyboard:space`. `midday validate
  conflict/a.input.yaml --json` refuses (exit 3, `input.conflict`) even
  though neither file lists the other — the cross-file collision check runs
  from either file's own directory.
- `profile/` — `base.input.yaml` (a base map) + `rebind.input_profile.yaml`
  (a clean runtime rebinding overlay: `jump` moves from space to enter) +
  `rebind_conflict.input_profile.yaml` (an overlay whose OWN two rebinds
  collide with each other, independent of any base map — exit 3,
  `input.conflict`). `apply_overlay()` (core/loader/loader.h) is the C++ path
  that combines an overlay against a specific base map and re-validates the
  result; there is no CLI verb for that composition (a game embeds it, spec
  section 13: the engine owns the data path, not the rebinding UI).

| fixture | `midday validate <file> --json` |
|---|---|
| `clean/a.input.yaml` | exit 0, 6 actions, 1 stick, 2 files |
| `conflict/a.input.yaml` | exit 3, `input.conflict` |
| `profile/rebind.input_profile.yaml` | exit 0, 1 rebind |
| `profile/rebind_conflict.input_profile.yaml` | exit 3, `input.conflict` |
