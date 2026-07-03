// testkit/codegen_corpus.h — the synthetic codegen corpus, shared by the
// bootstrap byte-goldens (tools/codegen_bootstrap/codegen_test.cpp) and the
// selfhost equivalence harness (ts/codegen/selfhost_test.cpp). This corpus
// IS the bootstrap-corpus half of the MILESTONE_0 "Codegen Bootstrap"
// contract: the self-hosted generator must byte-match the native tool on it
// before it is authoritative. It outlives the bootstrap tool's retirement.

#pragma once

#include <string_view>

namespace midday::testkit {

// One entry per section, exercising every emitter path the live corpus
// cannot yet reach (classes!): docs, defaults, flags, composites, methods.
inline constexpr std::string_view kCodegenSyntheticDocument = R"json({
 "format_version": 1,
 "engine_version": "9.9.9-test",
 "api_compat_hash": "00000000000000aa",
 "classes": [{"name": "health", "level": "scene", "doc": "Hit points.",
   "properties": [
     {"name": "max", "type": "float", "default": 100, "doc": "Cap."},
     {"name": "current", "type": "float", "flags": ["save"], "doc": "Now."},
     {"name": "tags", "type": "array<name>"}],
   "methods": [{"name": "damage", "params": [{"name": "amount", "type": "float"}],
     "returns": "float", "doc": "Apply damage; returns remaining.",
     "compat_hash": "00000000000000ab"}],
   "compat_hash": "00000000000000ac"}],
 "events": [{"name": "probe.fired", "level": "core", "doc": "A probe fired.",
   "payload": [
     {"name": "who", "type": "entity_ref", "doc": "The probe."},
     {"name": "strength", "type": "float", "doc": "Blast strength."},
     {"name": "path", "type": "array<vec3>", "doc": "Waypoints."}],
   "compat_hash": "00000000000000ad"}],
 "functions": [{"name": "mix", "level": "core",
   "params": [{"name": "a", "type": "float"}, {"name": "b", "type": "float"}],
   "returns": "float", "doc": "Blend a into b.", "compat_hash": "00000000000000ae"}],
 "verbs": [{"name": "probe", "summary": "fire the probe",
   "flags": [{"name": "count", "type": "int", "required": false, "doc": "How many."},
     {"name": "dry-run", "type": "bool", "required": false, "doc": "Rehearse only."}],
   "positionals": [{"name": "target", "type": "name", "required": true, "variadic": false,
     "doc": "What to hit."}],
   "compat_hash": "00000000000000af"}]
})json";

// Number-formatting edge corpus (api/CODEGEN.md "Numbers"): every default
// below is chosen to stress one branch of the core JSON writer that the
// self-hosted generator re-implements — int64 tokens (incl. both range
// ends and a value JS Number cannot hold), -0, beyond-int64 degradation to
// double, fixed/scientific selection ties, exact integer expansion of
// integer-valued doubles, denormal/huge doubles, and dump()ed defaults
// carrying '|', '"', and control characters into markdown cells verbatim.
inline constexpr std::string_view kCodegenNumberDocument = R"json({
 "format_version": 1,
 "engine_version": "0.0.0-numbers",
 "api_compat_hash": "00000000000000ba",
 "classes": [{"name": "numerics", "level": "scene", "doc": "Number-format edges.",
   "properties": [
     {"name": "half", "type": "float", "default": 0.5},
     {"name": "tenth", "type": "float", "default": 0.1},
     {"name": "accum", "type": "float", "default": 0.30000000000000004},
     {"name": "milli", "type": "float", "default": 0.0001},
     {"name": "micro", "type": "float", "default": 1e-06},
     {"name": "sub_micro", "type": "float", "default": 1e-07},
     {"name": "denormal", "type": "float", "default": 5e-324},
     {"name": "huge", "type": "float", "default": 1.7976931348623157e308},
     {"name": "sci", "type": "float", "default": 1e21},
     {"name": "whole", "type": "float", "default": 100.0},
     {"name": "neg_zero", "type": "float", "default": -0},
     {"name": "exact_int", "type": "int", "default": 9007199254740993},
     {"name": "int_max", "type": "int", "default": 9223372036854775807},
     {"name": "int_min", "type": "int", "default": -9223372036854775808},
     {"name": "beyond_int", "type": "float", "default": 9223372036854775808},
     {"name": "mantissa", "type": "float", "default": 12345.6789},
     {"name": "tuple", "type": "vec3", "default": [0.25, -0.125, 12345.6789]},
     {"name": "table", "type": "map<float>", "default": {"a|b": 2.5e-08, "line\nbreak": 3}},
     {"name": "label", "type": "string", "default": "quote\" pipe| tab\t"}],
   "methods": [{"name": "scale",
     "params": [{"name": "by", "type": "float", "default": 2.5e-08}],
     "returns": "float", "compat_hash": "00000000000000bb"}],
   "compat_hash": "00000000000000bc"}],
 "events": [],
 "functions": [{"name": "lerp3", "level": "core",
   "params": [{"name": "t", "type": "float", "default": 0.5}],
   "returns": "float", "compat_hash": "00000000000000bd"}],
 "verbs": [{"name": "bench", "summary": "number-default flags",
   "flags": [{"name": "rate", "type": "float", "required": false, "default": 2.5e-08,
     "doc": "Sample rate."}],
   "positionals": [],
   "compat_hash": "00000000000000be"}]
})json";

} // namespace midday::testkit
