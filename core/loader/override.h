// core/loader/override.h — the property-diff override engine (m1-scene-
// format, spec 4.2 "Override path grammar":
// `<machine-name>/<Region>/<State>/<ChildEntity>/<Component>`, "machines
// addressed by name, never index").
//
// Resolution walks a machine's regions/states/children tree BY NAME ONLY:
// no vector index, no declaration-order assumption, ever touches the
// result. A path's segments after the region are matched against the
// CURRENT container's state names one level at a time (region's top-level
// states, then that state's own substates, and so on) — never a flat
// "any state anywhere in the region" scan — so a path can only resolve
// through the SAME parent/child structure the machine file actually
// authored, and reordering/renaming siblings elsewhere in the tree can
// never change what a path resolves to (the exit-test #2 guarantee).
//
// Two leaf shapes, both authored in examples/warden/:
//   * `.../<State>/sequence`            -> a property diff onto that
//                                          state's SequenceDesc (duration,
//                                          end, loop — the AUTHORED scalar
//                                          fields; tracks/then are
//                                          structural, never diffable).
//   * `.../<State>/<Component>`         -> a property diff onto a
//     `.../<State>/<Child>/<Component>`    component this state (or a
//                                          named child entity under it)
//                                          owns directly (shallow JSON
//                                          merge into GenericComponentEntry
//                                          ::fields / StateComponentDesc
//                                          ::fields).
//
// A bad override path (unknown region/state/child/component, a `sequence`
// target on a state with none, an unrecognized sequence-diff field) is
// ALWAYS a hard "loader.bad_ref"/"loader.bad_value" refusal, in every mode
// — an override addresses something by name; if that name is wrong, it is
// an authoring bug, never a "content that doesn't exist yet" gap (Gap is
// reserved for component TYPE names and asset files, core/loader/gaps.h).

#pragma once

#include "core/base/error.h"
#include "core/loader/entity_format.h"
#include "core/loader/loader.h"
#include "core/loader/yaml.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace midday::loader {

struct OverrideParseResult {
    std::vector<OverrideEntry> entries;
    std::optional<base::Error> error;
};

// Parses an `override:` mapping (scene entity or machine-instance level):
// every key is a '/'-separated override path, every value a property-diff
// MAPPING (a scalar/list override value is meaningless here — there is no
// "replace the whole subtree" semantics anywhere in the grammar).
OverrideParseResult parse_override_block(const YamlNode& node, const std::string& file);

// Splits a machine-instance's overrides into the ones whose FIRST path
// segment names `machine_name` (the scene-level grammar, an entity may own
// more than one machine instance) and the rest. `require_prefix = false`
// is the entity/prefab-level grammar (already scoped to one instance; every
// entry's first segment is a REGION name directly, no machine-name hop).
struct SplitOverrides {
    std::vector<OverrideEntry> matched;   // machine-name segment stripped
    std::vector<OverrideEntry> unmatched; // did not name `machine_name`
};

SplitOverrides split_overrides_for_machine(const std::vector<OverrideEntry>& overrides,
                                           std::string_view machine_name);

struct ApplyOverridesResult {
    MachineFile machine;              // a COPY of `base` with overrides applied
    std::optional<base::Error> error; // engaged -> `machine` is meaningless
};

// Applies `overrides` (already stripped of any machine-name segment — see
// split_overrides_for_machine) onto a COPY of `base`. Never mutates the
// caller's cached MachineFile: the same machine file may be instanced by
// many entities, each with its own overrides. `origin_file` is the
// diagnostic origin (the scene/entity file the `override:` block was
// AUTHORED in — entry.line/col are positions in THAT file, not `base`'s).
ApplyOverridesResult apply_overrides(MachineFile base,
                                     const std::vector<OverrideEntry>& overrides,
                                     std::string_view origin_file);

} // namespace midday::loader
