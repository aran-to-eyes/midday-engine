# core/sequence

Entity-level sequences shipped INSIDE `core/statechart` (m0-sequences,
D-BUILD-057): a dope sheet is a state's body (spec §4.1 — "a state whose
body is a timeline"), spans close inside the A.2.1 exit chain, the playhead
is per-state runtime data, and `<state>.finished` rides the statechart's
emission path — a separate library here would have needed either a cyclic
dependency or leaked `MachineInstance` internals.

See `core/statechart/sequences.cpp`, `SequenceDesc` in
`core/statechart/machine_desc.h`, `RtSheet` in
`core/statechart/instance.h`, and the `sequence.*` selftests.

This directory is reserved for scene-owned DIRECTOR sequences
(m5-director-sequences): the same two track kinds, choreographing MANY
entities plus camera and audio.
