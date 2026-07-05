// core/loader/entity_format.h — m1-scene-format additions to the loader's
// data model: the generic (name + opaque JSON fields) component shape used
// wherever a component list is NOT the M0 native {Transform, Collider,
// RigidBody} set (loader.h's typed ComponentSet keeps owning that surface
// unchanged, exit-test #4), the property-diff override entry, and the
// generic `{uid?, path}` asset-reference shape the new prefab/model/
// attachment grammar uses.
//
// Split out of loader.h (which stays close to the 500-line ratchet) rather
// than grown in place — one loader, many headers, spec's "extend in place"
// still holds: core/loader/loader.h includes this file (plus defines the
// entity/prefab-FILE aggregate itself, which needs the already-complete
// MachineFile type) and stays the ONLY public surface (formats/loader_yaml.md).

#pragma once

#include "core/base/json.h"
#include "core/base/name.h"

#include <optional>
#include <string>
#include <vector>

namespace midday::loader {

// One `{Name: {field: value, ...}}` component entry outside the M0 native
// vocabulary: a scene/prefab entity's non-native inline component, an
// entity file's `base:` entry (EVERY base component is generic — see
// entity_load.cpp; splitting the native three back out into typed data is
// m1-prefab-spawn's job, not this node's), or a state child's
// `components:` entry. `fields` is the property mapping verbatim
// (yaml_to_json), never interpreted here — the SAME "pure data, no runtime
// type yet" contract core/statechart::StateComponentDesc uses one layer up.
struct GenericComponentEntry {
    base::Name type;
    base::Json fields = base::Json::object();
};

// One `<path>: {field: value, ...}` entry of an `override:` mapping
// (spec 4.2 "Override path grammar"). `path` is the RAW '/'-separated
// text, resolved by core/loader/override.h against a specific machine's
// regions/states BY NAME — never split or interpreted here.
struct OverrideEntry {
    std::string path;
    base::Json diff = base::Json::object();
    int line = 0;
    int col = 0;
};

// A generic `{uid?, path}` asset reference as the loader resolves it:
// `path_resolved` is the project-root-relative filesystem path; `exists`
// records whatever the resolving loader found on disk. Kept separate from
// MachineFile's own `instance:` resolution (which stays hard-required,
// M0's existing behavior) — this shape is for the NEW, lenient-by-request
// asset kinds this node introduces (prefab/model/attachment refs).
struct AssetRefDesc {
    std::string uid; // as authored; empty iff no `uid:` field
    bool has_uid = false;
    std::string path_authored; // as authored (project-root-relative)
    std::string path_resolved; // resolved against the referencing file's root_dir
    bool exists = false;
    int line = 0;
    int col = 0;
};

// One `attachments:` entry (a socket on an entity/prefab file): an asset
// (`of:`) mounted at a named socket, optionally carrying its own nested
// entity (`entity: {prefab: ...}`, e.g. warden_mace.entity.yaml — a full
// prefab reference, not resolved/spawned by this node; m1-prefab-spawn's
// remit). Both refs are asset-kind-lenient (missing -> Gap, never a hard
// refusal) — sockets/attachments are new grammar with no M0 precedent to
// stay backward-compatible with.
struct AttachmentDesc {
    base::Name socket;
    AssetRefDesc of;
    std::optional<AssetRefDesc> entity_prefab; // `entity: {prefab: ...}`, when present
    int line = 0;
};

} // namespace midday::loader
