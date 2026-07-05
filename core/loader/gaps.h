// core/loader/gaps.h — the UNRESOLVED-REFERENCE report (m1-scene-format).
//
// The Warden stress-test (examples/warden/) authors scene/prefab/machine
// content that references components, scripts, and assets which do not
// exist in the engine YET (MeshRenderer, Perception, NavFollow, ...;
// player.entity.yaml, *.model.yaml, states/chase.ts, ...). The permanent
// loader's default posture stays exactly what M0 established: STRICT,
// hard-refuse on anything it cannot honor (loader.bad_ref / .unknown_key,
// exit 3) — every existing caller (`midday run`, `midday validate`, every
// m0/m1 doctest) keeps that behavior UNCHANGED, because the strict path
// (`lenient = false`, the default everywhere) never constructs a Gap.
//
// A SEPARATE, opt-in LENIENT mode (threaded through load_scene /
// load_machine_file / load_entity_file as a trailing `lenient` argument)
// downgrades exactly the categories that are legitimately "content this
// engine doesn't implement yet" — unknown component types, missing state
// scripts, missing prefab/model/attachment asset files — from a hard
// refusal into a collected Gap, so the file still PARSES and the caller
// (`midday scene print`, the only lenient caller in this milestone) can
// report every gap honestly instead of either crashing or pretending
// completion. Genuine authoring mistakes (bad override paths, unknown YAML
// keys, type errors, missing MACHINE files) are never downgraded — they stay
// hard refusals even in lenient mode, because they are not "future work",
// they are bugs.
#pragma once

#include "core/base/json.h"

#include <string>
#include <utility>
#include <vector>

namespace midday::loader {

// One unresolved reference, discovered while parsing in LENIENT mode.
struct Gap {
    std::string kind; // "component" | "script" | "prefab" | "model" | "entity"
    std::string what; // the unresolved name/path, as authored
    std::string file; // the file the reference was authored in
    int line = 0;
    int col = 0;
    std::string detail; // human-readable explanation

    [[nodiscard]] base::Json to_json() const {
        base::Json out = base::Json::object();
        out.set("kind", kind);
        out.set("what", what);
        out.set("file", file);
        out.set("line", static_cast<std::int64_t>(line));
        out.set("col", static_cast<std::int64_t>(col));
        out.set("detail", detail);
        return out;
    }
};

inline void append_gaps(std::vector<Gap>& into, std::vector<Gap> from) {
    into.insert(
        into.end(), std::make_move_iterator(from.begin()), std::make_move_iterator(from.end()));
}

} // namespace midday::loader
