// codegen.* selftests — written first (test-first): the synthetic-corpus
// byte goldens (the scaffolding contract m0-codegen-selfhost must match),
// dual-run byte equality on the LIVE document, the d.ts structural shape
// check, escape rules, and malformed-input structured errors with exit-3
// semantics (api/CODEGEN.md "Validation order").

#include "api/engine_api.h"
#include "cli/help.h"
#include "cli/verb.h"
#include "testkit/doctest.h"
#include "testkit/codegen_corpus.h"
#include "testkit/doctest_unwrap.h"
#include "tools/codegen_bootstrap/codegen.h"

#include <ostream> // MSVC: doctest stringifies string_view via operator<< (D-BUILD-016)
#include <string>
#include <string_view>

namespace api = midday::api;
namespace base = midday::base;
namespace cli = midday::cli;
namespace codegen = midday::codegen;

using base::Json;

namespace {

// The synthetic corpus (testkit/codegen_corpus.h) is shared with the
// selfhost equivalence harness; the byte goldens below pin ITS output.
constexpr std::string_view kSyntheticDocument = midday::testkit::kCodegenSyntheticDocument;

constexpr std::string_view kGoldenDts =
    R"dts(// engine.d.ts -- GENERATED from engine_api.json. DO NOT EDIT.
// engine_version 9.9.9-test, api_compat_hash 00000000000000aa (signatures only; docs excluded).
// Formatting rules + the TypeDesc -> TypeScript mapping table: api/CODEGEN.md.
// Structural (pre-tsc) validation conventions: formats/engine_dts.meta.md.

declare namespace midday {
    // -- Value types (fixed preamble; scalar TypeDesc spellings map per api/CODEGEN.md) --

    /** TypeDesc "vec2": 2D float vector. */
    interface Vec2 {
        x: number;
        y: number;
    }

    /** TypeDesc "vec3": 3D float vector. */
    interface Vec3 {
        x: number;
        y: number;
        z: number;
    }

    /** TypeDesc "vec4": 4D float vector. */
    interface Vec4 {
        x: number;
        y: number;
        z: number;
        w: number;
    }

    /** TypeDesc "quat": rotation quaternion; JSON spelling [x, y, z, w]. */
    interface Quat {
        x: number;
        y: number;
        z: number;
        w: number;
    }

    /** TypeDesc "color": linear RGBA; JSON spelling [r, g, b, a]. */
    interface Color {
        r: number;
        g: number;
        b: number;
        a: number;
    }

    /** TypeDesc "entity_ref": generational entity handle; a stale handle reads alive == false. */
    interface EntityRef {
        readonly alive: boolean;
    }

    /** TypeDesc "asset_ref": project-root-relative asset path. */
    type AssetRef = string;

    // -- Reflected classes (engine_api.json "classes", registration order) --

    /** Hit points. */
    interface Health {
        /** Cap. */
        max: number;
        /** Now. */
        current: number;
        tags: string[];
        /** Apply damage; returns remaining. */
        damage(amount: number): number;
    }

    /** Class name -> reflected interface. */
    interface Classes {
        "health": Health;
    }

    // -- Event payloads (engine_api.json "events", registration order) --

    /** A probe fired. */
    interface ProbeFiredEvent {
        /** The probe. */
        who: EntityRef;
        /** Blast strength. */
        strength: number;
        /** Waypoints. */
        path: Vec3[];
    }

    /** Event name -> payload type. */
    interface EventPayloads {
        "probe.fired": ProbeFiredEvent;
    }

    // -- Expression functions (engine_api.json "functions"): expression-language signatures for editor tooling, not TS-callable --

    namespace expr {
        /** Blend a into b. */
        function mix(a: number, b: number): number;
    }

    // -- CLI verbs (engine_api.json "verbs"): midday argv schemas as types, manifest order --

    /** fire the probe */
    interface ProbeVerbArgs {
        /** How many. */
        count?: number;
        /** Rehearse only. */
        "dry-run"?: boolean;
        /** What to hit. */
        target: string;
    }

    /** Verb name -> parsed-argument type. */
    interface VerbArgsByName {
        "probe": ProbeVerbArgs;
    }
}
)dts";

constexpr std::string_view kGoldenManifest =
    R"json({"format_version":1,"api_compat_hash":"00000000000000aa","value_types":[{"spelling":"bool","json":"boolean"},{"spelling":"int","json":"integer"},{"spelling":"float","json":"number"},{"spelling":"string","json":"string"},{"spelling":"name","json":"string"},{"spelling":"vec2","json":"number_tuple","size":2},{"spelling":"vec3","json":"number_tuple","size":3},{"spelling":"vec4","json":"number_tuple","size":4},{"spelling":"quat","json":"number_tuple","size":4},{"spelling":"color","json":"number_tuple","size":4},{"spelling":"entity_ref","json":"string"},{"spelling":"asset_ref","json":"string"},{"spelling":"array","json":"array_of_element"},{"spelling":"map","json":"object_of_element"}],"events":[{"name":"probe.fired","payload":[{"name":"who","type":"entity_ref"},{"name":"strength","type":"float"},{"name":"path","type":"array<vec3>"}]}],"expr_functions":[{"name":"mix","params":["float","float"],"returns":"float"}],"formats":[]}
)json";

constexpr std::string_view kGoldenBindings =
    R"json({"format_version":1,"api_compat_hash":"00000000000000aa","expr_functions":[{"name":"mix","level":"core","params":[{"name":"a","type":"float"},{"name":"b","type":"float"}],"returns":"float","compat_hash":"00000000000000ae"}],"events":[{"name":"probe.fired","level":"core","payload":[{"name":"who","type":"entity_ref"},{"name":"strength","type":"float"},{"name":"path","type":"array<vec3>"}],"compat_hash":"00000000000000ad"}],"classes":[{"name":"health","level":"scene","properties":[{"name":"max","type":"float","default":100},{"name":"current","type":"float","flags":["save"]},{"name":"tags","type":"array<name>"}],"methods":[{"name":"damage","params":[{"name":"amount","type":"float"}],"returns":"float","compat_hash":"00000000000000ab"}],"compat_hash":"00000000000000ac"}],"batch_envelope":{"envelope_version":0,"status":"placeholder","doc":"Reserved: m0-batch-bindings designs the real batch envelope (per-query SoA views backed by typed arrays, one segment per component column, pooled math slots, per-tick crossing/GC counters). views stays empty and envelope_version stays 0 until then; refuse envelope_version 0 for actual batching.","views":[]}}
)json";

constexpr std::string_view kGoldenDocs = R"md(# Midday Engine API reference

GENERATED from engine_api.json. DO NOT EDIT.
Signature compat hashes are XXH3-64 over signature-only JSON (docs excluded).

- engine_version: `9.9.9-test`
- api_compat_hash: `00000000000000aa`

## Classes

### `health`

Hit points.

- level: `scene`
- compat_hash: `00000000000000ac`

| property | type | default | flags | doc |
| --- | --- | --- | --- | --- |
| `max` | `float` | `100` |  | Cap. |
| `current` | `float` |  | save | Now. |
| `tags` | `array<name>` |  |  |  |

Methods:

- `damage(amount: float) -> float` (compat_hash `00000000000000ab`) -- Apply damage; returns remaining.

## Events

### `probe.fired`

A probe fired.

- level: `core`
- compat_hash: `00000000000000ad`

| field | type | doc |
| --- | --- | --- |
| `who` | `entity_ref` | The probe. |
| `strength` | `float` | Blast strength. |
| `path` | `array<vec3>` | Waypoints. |

## Expression functions

### `mix(a: float, b: float) -> float`

Blend a into b.

- level: `core`
- compat_hash: `00000000000000ae`

## CLI verbs

### `midday probe`

fire the probe

- compat_hash: `00000000000000af`

Flags:

| flag | type | required | default | doc |
| --- | --- | --- | --- | --- |
| `--count` | `int` | no |  | How many. |
| `--dry-run` | `bool` | no |  | Rehearse only. |

Positionals:

| positional | type | required | variadic | doc |
| --- | --- | --- | --- | --- |
| `target` | `name` | yes | no | What to hit. |
)md";

Json load_ok(std::string_view bytes) {
    codegen::LoadResult loaded = codegen::load_document(bytes, "<test>");
    REQUIRE_MESSAGE(!loaded.error.has_value(),
                    (loaded.error ? loaded.error->message : std::string()));
    return std::move(loaded.document);
}

base::Error load_error(std::string_view bytes) {
    codegen::LoadResult loaded = codegen::load_document(bytes, "<test>");
    const base::Error error = midday::testkit::unwrap(loaded.error);
    CHECK(codegen::exit_code_for(error) == 3);
    return error;
}

base::Error code_only(std::string_view code) {
    base::Error error;
    error.code = std::string(code);
    return error;
}

Json live_document() {
    const api::BootRegistry boot;
    Json schemas = Json::array();
    for (const cli::VerbSpec* spec : cli::verbs())
        schemas.push(cli::verb_schema(*spec));
    return api::build_document(boot.registry, schemas, "test");
}

} // namespace

TEST_CASE("codegen.golden: synthetic corpus pins all four outputs byte-for-byte") {
    const Json document = load_ok(kSyntheticDocument);
    CHECK(codegen::emit_dts(document) == kGoldenDts);
    CHECK(codegen::emit_manifest(document) == kGoldenManifest);
    CHECK(codegen::emit_bindings(document) == kGoldenBindings);
    CHECK(codegen::emit_docs(document) == kGoldenDocs);
    CHECK(codegen::dts_shape_errors(kGoldenDts, document).empty());
}

TEST_CASE("codegen.live: current document generates dual-run identical, shape-valid outputs") {
    const Json document = live_document();
    const codegen::Outputs first = codegen::generate(document);
    const codegen::Outputs second = codegen::generate(document);
    CHECK(first.dts == second.dts);
    CHECK(first.manifest == second.manifest);
    CHECK(first.docs == second.docs);
    CHECK(first.bindings == second.bindings);

    // d.ts structural shape (formats/engine_dts.meta.md): balanced braces,
    // every event/function/verb declared, no unresolved-generation tokens.
    const std::vector<std::string> shape = codegen::dts_shape_errors(first.dts, document);
    CHECK_MESSAGE(shape.empty(), (shape.empty() ? std::string() : shape.front()));

    // The manifest re-parses, carries the same hash, and covers every event.
    const Json::ParseResult manifest = Json::parse(first.manifest, "schema_manifest.json");
    REQUIRE(static_cast<bool>(manifest));
    CHECK(manifest.value.find("api_compat_hash")->as_string() ==
          document.find("api_compat_hash")->as_string());
    CHECK(manifest.value.find("events")->elements().size() ==
          document.find("events")->elements().size());
    CHECK(manifest.value.find("expr_functions")->elements().size() ==
          document.find("functions")->elements().size());

    // Round trip: the document's own dump loads back identically.
    const Json reloaded = load_ok(document.dump());
    CHECK(codegen::generate(reloaded).dts == first.dts);
}

TEST_CASE("codegen.envelope: a CLI dump envelope (--json) is unwrapped to the document") {
    Json envelope = Json::object();
    envelope.set("ok", true);
    envelope.set("verb", "api");
    envelope.set("exit_code", 0);
    envelope.set("api", load_ok(kSyntheticDocument));
    const Json document = load_ok(envelope.dump());
    CHECK(codegen::emit_dts(document) == kGoldenDts);
}

TEST_CASE("codegen.errors: malformed inputs are structured validation errors (exit 3)") {
    CHECK(load_error("{ nope").code == "json.parse");
    CHECK(load_error(R"({"format_version": 99})").code == "api.malformed");

    // Unknown type spelling, with the offending spelling in the details.
    std::string bad(kSyntheticDocument);
    const std::string needle = "\"entity_ref\"";
    bad.replace(bad.find(needle), needle.size(), "\"flarb\"");
    const base::Error unknown_type = load_error(bad);
    CHECK(unknown_type.code == "codegen.unknown_type");
    CHECK(unknown_type.details.find("type")->as_string() == "flarb");

    // Entry shape the emitters rely on: an event without a payload array.
    std::string no_payload(kSyntheticDocument);
    const std::string payload_key = "\"payload\"";
    no_payload.replace(no_payload.find(payload_key), payload_key.size(), "\"payloax\"");
    CHECK(load_error(no_payload).code == "codegen.malformed");

    // Exit class mapping is pinned (api/CODEGEN.md).
    CHECK(codegen::exit_code_for(code_only("usage.unknown_flag")) == 2);
    CHECK(codegen::exit_code_for(code_only("usage.unexpected_argument")) == 2);
    CHECK(codegen::exit_code_for(code_only("codegen.io.write")) == 1);
    CHECK(codegen::exit_code_for(code_only("codegen.selfcheck")) == 1);
    CHECK(codegen::exit_code_for(code_only("codegen.io")) == 3);
    CHECK(codegen::exit_code_for(code_only("api.malformed")) == 3);
}

TEST_CASE("codegen.helpers: pascal_case, the TypeDesc -> TS mapping table, escapes") {
    CHECK(codegen::pascal_case("trigger.entered") == "TriggerEntered");
    CHECK(codegen::pascal_case("length_squared") == "LengthSquared");
    CHECK(codegen::pascal_case("probe") == "Probe");
    CHECK(codegen::pascal_case("a-b.c_d") == "ABCD");

    CHECK(codegen::ts_type("bool") == "boolean");
    CHECK(codegen::ts_type("int") == "number");
    CHECK(codegen::ts_type("float") == "number");
    CHECK(codegen::ts_type("string") == "string");
    CHECK(codegen::ts_type("name") == "string");
    CHECK(codegen::ts_type("vec2") == "Vec2");
    CHECK(codegen::ts_type("vec3") == "Vec3");
    CHECK(codegen::ts_type("vec4") == "Vec4");
    CHECK(codegen::ts_type("quat") == "Quat");
    CHECK(codegen::ts_type("color") == "Color");
    CHECK(codegen::ts_type("entity_ref") == "EntityRef");
    CHECK(codegen::ts_type("asset_ref") == "AssetRef");
    CHECK(codegen::ts_type("array<vec3>") == "Vec3[]");
    CHECK(codegen::ts_type("map<string>") == "Record<string, string>");
    CHECK(codegen::ts_type("array<map<int>>") == "Record<string, number>[]");
    CHECK(codegen::ts_type("map<array<float>>") == "Record<string, number[]>");

    CHECK(codegen::jsdoc_escape("a */ b\nc") == "a *\\/ b c");
    CHECK(codegen::cell_escape("dump | diff\nx") == "dump \\| diff x");
}

TEST_CASE("codegen.shape: the d.ts checker actually falsifies broken output") {
    const Json document = load_ok(kSyntheticDocument);
    std::string dts(kGoldenDts);

    // Unbalanced brace on a non-comment line.
    CHECK(!codegen::dts_shape_errors(dts + "}\n", document).empty());

    // A declared event interface goes missing.
    std::string missing = dts;
    const std::string iface = "interface ProbeFiredEvent";
    missing.replace(missing.find(iface), iface.size(), "interface Renamed");
    CHECK(!codegen::dts_shape_errors(missing, document).empty());

    // Unresolved-generation token on a code line.
    CHECK(!codegen::dts_shape_errors(dts + "type X = TODO;\n", document).empty());

    // Comment lines are exempt from token + brace scanning.
    CHECK(codegen::dts_shape_errors(dts + "// TODO { later\n", document).empty());
}
