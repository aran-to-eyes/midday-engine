# uid-system fixture corpus (m1-uid-system)

Self-contained `{uid, path}` dual-write fixtures (spec lines 363-368),
exercising `midday check`/`midday mv` through the real CLI. `fixture.demo.yaml`
is a purpose-built ref-shape document (`sprite: {uid, path}`), NOT a real
scene/machine file — same call m1-strict-yaml made for `widget.schema.json`,
to avoid pre-empting `m1-scene-format`'s scene/machine grammar.

- `clean/` — one asset (`assets/widget.asset`), its committed `.uid`
  sidecar, and a reference that already cites the CORRECT uid. The uid was
  minted by the real tool, never hand-typed: `fixture.demo.yaml` originally
  had a path-only `sprite: {path: assets/widget.asset}` ref, and
  `midday check clean --fix` attached the uid you see committed here
  (`uid://1izhy6gbay48m`) — regenerate the same way if this fixture is ever
  rebuilt from scratch. Used by the exit-test #1 (move) and #3 (cache
  regeneration) scenarios: `scripts/verify.sh` copies this tree to a scratch
  directory before mutating it, so the committed fixture is never touched by
  a test run.
- `hand_minted/` — the SAME asset shape, but `fixture.demo.yaml`'s ref cites
  `uid://0000000000001`, a well-formed uid with NO `.uid` sidecar anywhere
  under this directory. This is exit-test #2: `midday check hand_minted`
  refuses (exit 3, `check.invalid_ref`) because a uid is only ever legitimate
  when a committed sidecar backs it — a fabricated/copy-pasted uid can never
  "pass" no matter how well-formed its text is.

| fixture | `midday check <dir> --json` |
|---|---|
| `clean` | exit 0, one `clean` finding |
| `hand_minted` | exit 3, `check.invalid_ref`, one `invalid` finding |

`.midday-cache/` directories that appear under either fixture while
exercising them are scratch output (regenerable, gitignored) — never commit
one; `scripts/verify.sh`'s "m1-uid-system" step always works against a
`build/dev/...` copy, never these committed directories in place.
