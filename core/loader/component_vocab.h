// core/loader/component_vocab.h — the component-NAME vocabulary a scene/
// entity/machine file's component entries are checked against (m1-scene-
// format integrating m1-ts-components): the M0 native set (Transform,
// Collider, RigidBody — already fully typed by core/loader::ComponentSet,
// unaffected by this file) plus, optionally, the NAMES declared in a
// project-level component-schema manifest (`midday script extract --out
// <path>`, cli/verbs/script.cpp) — Health, DamageOnTouch, and friends.
//
// Scope, deliberately narrow: this is a NAME lookup, not a field-level
// validator. A generic component entry's fields are stored verbatim
// (core/loader::GenericComponentEntry, base::Json) and never type-checked
// against the manifest's field list here — that is real work for whichever
// node actually instantiates TS components onto entities (out of scope,
// brief SCOPE section). Resolving the type NAME is exactly the boundary
// exit-test #5 needs: Health/DamageOnTouch resolve when a manifest is
// supplied (they are real, already-authored TS components,
// examples/warden/components/); Perception/MeshRenderer/NavFollow/
// StaggerTimer/Spline/VirtualCamera never do, because no manifest — now or
// later, until someone writes them — will ever declare those names.
#pragma once

#include "core/base/error.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace midday::loader {

// Native, always-known component names — the M0 vocabulary (loader.h
// ComponentSet). Kept as a free function (not a class member) so the
// natives list has exactly one spelling, shared by scene- and state-level
// checks alike.
[[nodiscard]] bool is_native_component(std::string_view name);

struct ComponentVocab {
    std::vector<std::string> extracted; // names from a project component manifest

    [[nodiscard]] bool known(std::string_view name) const {
        if (is_native_component(name))
            return true;
        for (const std::string& candidate : extracted)
            if (candidate == name)
                return true;
        return false;
    }
};

struct ComponentVocabLoadResult {
    ComponentVocab vocab;
    std::optional<base::Error> error;
};

// Reads a project component-schema manifest (the `{"format_version":2,
// "components":[{"name":...},...]}` document `midday script extract`
// writes; format 1, pre-event_bindings, reads identically — the NAME walk
// never touches versioned members) and extracts just the component NAMES.
// `path` is optional: an
// empty path returns an empty (native-only) vocab, not an error — callers
// that have no manifest to offer (the common case until a project wires
// one) still get well-defined behavior.
ComponentVocabLoadResult load_component_vocab(const std::string& path);

} // namespace midday::loader
