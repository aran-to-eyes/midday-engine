// core/loader/machine_emit.h — the CANONICAL machine-file serializer
// (m1-scene-format, spec 4.2 "`on:` on a state is sugar for a Transition
// component pair list; canonical serialization emits the component form").
// The counterpart of machine_load.cpp/machine_parts.cpp: turns an already-
// loaded MachineFile back into a YamlNode tree (core/loader/yaml_build.h)
// ready for core/loader/yaml_emit.h — `midday scene print --full` on a
// `*.machine.yaml` file uses this for its round-trip-stable canonical text
// (exit-test #1/#3).
//
// Desugaring, always applied (never conditional on how the source was
// authored): `on:` -> `Transition:` (the identical pair list; `Transition:`
// is an accepted alias machine_load.cpp/machine_parts.cpp parses
// identically, so re-loading the emitted text reproduces the SAME
// MachineDesc — the round-trip guarantee). `then:` never reappears: its
// effect already landed as an ordinary pair in `state.transitions` at load
// time (D-BUILD-057), so there is nothing left to re-derive it from, by
// design (SPEC-GAP #5's call: canonical form never re-introduces sugar).
//
// "Full" default-filling: `history:` (state/region) and a pair's
// `priority:` always render explicitly, even when they hold the type's
// zero value — spec 369's "full-state visibility". A pair's `if:` renders
// only when non-empty (there is no non-empty "default condition" to show;
// omission already IS the canonical spelling of "unconditional"). A pair's
// `key:` never reappears: TransitionDesc does not retain which channel a
// pair listened on (event names are matched across every subscribed
// channel, D-BUILD-046) — a deliberate, documented loss the canonical form
// accepts (recorded in the node's design-decision log).
#pragma once

#include "core/loader/loader.h"
#include "core/loader/yaml.h"

namespace midday::loader {

// Builds the canonical tree for one machine file's CURRENT MachineDesc +
// children + scripts (already loaded — this never re-reads or re-resolves
// anything). `machine.path`/scripts are not reflected in the tree (script
// refs render as authored `script:` strings, taken from `machine.scripts`).
[[nodiscard]] YamlNode machine_to_yaml(const MachineFile& machine);

} // namespace midday::loader
