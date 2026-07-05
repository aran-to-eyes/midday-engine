// core/loader/generic_components.h — the ONE `{Name: {field: value, ...}}`
// component-list parser (m1-scene-format), reused by every site that needs
// it: a scene entity's non-native inline components, an entity/prefab
// file's `base:` list, a machine state's own `components:` list, and a
// state child's `components:` list (spec 4.1 "states owning component
// sets" + "machines are prefab subtrees"). One engine, never four
// copy-pasted loops — the jscpd ratchet holds it to that.
//
// `vocab` is the M0 native set (Transform/Collider/RigidBody,
// component_vocab.h) plus any extracted TS component names; an unknown
// name is a Gap (core/loader/gaps.h) when `lenient`, a hard
// "loader.unknown_key" refusal otherwise — every existing/default caller
// (lenient defaults to false everywhere) keeps M0's exact behavior.
#pragma once

#include "core/base/error.h"
#include "core/loader/component_vocab.h"
#include "core/loader/gaps.h"
#include "core/loader/yaml.h"

#include <optional>
#include <string_view>
#include <vector>

namespace midday::loader {

template <typename ComponentEntry> struct GenericComponentsResult {
    std::vector<ComponentEntry> components;
    std::vector<Gap> gaps;            // lenient mode only
    std::optional<base::Error> error; // engaged -> components/gaps meaningless
};

// Declared here, DEFINED (and explicitly instantiated for the two entry
// types callers use — statechart::StateComponentDesc and
// GenericComponentEntry) in generic_components.cpp: the template body needs
// core/loader/parse_util.h's detail:: helpers, which stay internal to
// core/loader and are not exposed through this header.
template <typename ComponentEntry>
GenericComponentsResult<ComponentEntry> parse_generic_components(const YamlNode& node,
                                                                 std::string_view file,
                                                                 const ComponentVocab& vocab,
                                                                 bool lenient);

} // namespace midday::loader
