// core/loader/project_schema.h — the two project-level format schemas
// `midday new` scaffolds and `midday validate` checks (m1-project-new, spec
// lines 382-383): `midday.project.yaml` (project config: name, main scene,
// input map reference, named collision layers, physics/CI-determinism
// defaults — spec lines 306, 508, 606's "visible in the project file,
// adjustable per project") and `midday.import.yaml` (the asset import
// policy: convention via glob-scoped defaults, spec lines 379-385).
//
// Both ride the EXISTING generic engine (core/loader/format_schema.h) —
// no per-format C++ validation code, exactly like a schema_manifest.json
// `formats[]` entry — but neither is REGISTERED there: that manifest is
// m1-scene-format's exclusive territory (scene/machine/prefab only, per its
// own committed `formats: []` placeholder). These two schemas are compiled-
// in, self-contained format-entry documents instead (the same JSON shape
// `--schema-file` accepts, just embedded rather than read from disk) — a
// THIRD way to reach the generic engine, alongside `--schema-file` and
// `--schema <name>`).
//
// FLAT FIELDS ONLY: format_schema.h's engine validates a top-level field
// list (scalar / array<T> / map<T>), never a nested per-key sub-schema — so
// "physics defaults" are individual top-level scalars (physics_gravity,
// physics_fixed_hz), not a nested `physics: {...}` block, and the import
// policy's glob-scoped rules are a `map<string>` (glob -> import-kind
// string) rather than an array of {glob, kind} objects. Both read cleanly
// as ordinary YAML either way; the flat shape is what the reused engine can
// check without inventing a second validation framework.

#pragma once

#include "core/loader/format_schema.h"

namespace midday::loader {

// `midday.project.yaml`, format 1. Fields: name (string, required),
// main_scene (asset_ref, required — project-root-relative path to the
// scene `midday run` loads by default), input_map (asset_ref — a
// *.input.yaml at the project root; action maps "live in project config"
// spec line 508 by REFERENCE, since the actual grammar is m1-input-actions'
// own reused format), collision_layers (array<string> — named project data,
// spec line 306; declaration order is bit-position order), physics_gravity
// (vec3), physics_fixed_hz (int), seed (int — the CI-determinism default
// `midday run --seed` takes when a project-aware caller wires it through;
// this node scaffolds the value, it does not wire a consumer — see
// cli/verbs/new.cpp's design notes).
const FormatSchema& project_config_schema();

// `midday.import.yaml`, format 1. Fields: default_import (string, enum:
// mesh|texture|audio|raw — the fallback classification when no glob rule
// matches) and rules (map<string> — glob pattern -> import-kind string,
// spec lines 379-383's "glob-scoped defaults"). Per-asset OVERRIDE sidecars
// ("a per-asset sidecar exists only where an asset deviates", spec line
// 383) are a separate, per-asset concept this node does not scaffold: a
// fresh project has no assets yet for one to override.
const FormatSchema& import_policy_schema();

} // namespace midday::loader
