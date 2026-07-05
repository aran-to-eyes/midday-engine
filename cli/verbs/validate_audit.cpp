// cli/verbs/validate_audit.cpp — validate_audit.h: `midday validate <dir>
// --audit-missing` (m1-warden-contract-audit).
//
// The Warden corpus (examples/warden/) is m1-scene-format's normative
// example: format-complete, content-incomplete on purpose (core/loader/
// gaps.h). `scene print --full` already PARSES it and reports every
// unresolved reference as a raw Gap — component names the vocab doesn't
// know, missing state scripts, missing prefab/entity files. This verb
// REFINES those raw gaps into the project's KNOWN-COMPLETION MANIFEST: the
// exact set of files/components/wiring a later milestone (m4-warden-
// assets-complete) still owes the corpus, no more and no fewer than that.
// Three refinements over the raw gap list, each a design decision:
//
// 1. Present TS components resolve. `examples/warden/components/*.ts`
//    (Health, DamageOnTouch) are real, authored content — not missing.
//    `script check` proves each file typechecks + lints clean (the ONE
//    present-file gate this audit owns for TS, mirroring scripts/verify.sh's
//    own "ts components" step); the component NAME itself is read straight
//    off the `@component()`-decorated class identifier (component_class_name
//    below) rather than through `script extract`'s full field/method schema
//    walk. That walk requires every method parameter to carry a type its
//    (separate) reflect-TypeDesc table recognizes; damage_on_touch.ts's
//    `onEvent(ev: import('midday').TriggerEntered)` is a real, typechecked
//    engine type (TriggerEntered ships in api/engine.d.ts — the corpus's own
//    SPEC-GAP #6 comment) that the walk's qualified-import-type handling
//    does not special-case, so `extract()` reports schema.unresolved_type
//    and returns no component at all for an otherwise-clean file (see
//    ts/toolchain/driver.js's typeFromAnnotation). Fixing that walk is
//    m1-ts-components' remit; this audit only needs the declared NAME
//    (component_vocab.h's own "NAME lookup, not a field-level validator"
//    scope), so it does not depend on the fuller mechanism at all.
//
// 2. Engine-roadmap component names are OUT OF SCOPE, not missing.
//    MeshRenderer/Spline/VirtualCamera are unresolved component names too,
//    but they are documented, spec-normative FUTURE NATIVE primitives
//    (MIDDAY_ENGINE_SPEC.md: "Splines are a scene primitive"; "Camera
//    direction system ... virtual cameras as components"; mesh rendering is
//    core engine surface) tracked by the plan generally — a rendering/camera
//    milestone's debt, not this ONE example's. Perception/NavFollow/
//    StaggerTimer appear nowhere in the spec: they are bespoke Warden AI
//    behavior components the project itself must author (exactly like
//    Health/DamageOnTouch already are) — m4-warden-assets-complete's own
//    deliverable list names these three alongside chase.ts/staggered.ts,
//    never MeshRenderer/Spline/VirtualCamera. Native ECS gaps unrelated to
//    content at all (kind "statechart": SlashAttack's span-scoped substate
//    activation) are excluded the same way — an engine capability gap, not
//    a Warden-corpus TODO. All excluded gaps are still reported, under
//    `out_of_scope`, for transparency — nothing is silently dropped.
//
// 3. Nested asset refs inside GENERIC (opaque) components are refs too.
//    MeshRenderer's `model: {uid, path}` is invisible to the loader's own
//    Gap machinery (a generic component's fields are opaque JSON to it,
//    core/loader/generic_components.h) — so warden_body.model.yaml and
//    arena_floor.model.yaml never surface as raw gaps at all. This audit
//    reuses core/loader/asset_ref.h's format-agnostic `{uid?, path}` ref-
//    shape scanner (already `midday check`'s mechanism) over every *.yaml
//    file in the corpus to catch exactly these, deduplicated against
//    whatever the loader already reported natively (prefab/entity refs).
//
// A fourth item — `goldens/warden_dead.png` — is referenced only from a
// TS test's `toMatchGolden('...')` literal (`midday/testkit` does not exist
// yet); golden_literal() below reads that one literal argument straight off
// the source text (not a TS walk) and resolves it PROJECT-ROOT-relative,
// the testkit convention this corpus's one call site uses. A fifth item —
// the Animator rig binding warden_body.model.yaml implies (Aurora D-19) —
// has no file/component identity to discover at all; it is recorded as a
// `wiring` note whenever warden_body.model.yaml is among the missing
// models (states/slash_attack.ts already assumes an Animator component
// 'midday' does not export yet).
//
// Every missing/out-of-scope path is reported root-relative, forward-slash
// (D-BUILD-113) — `lexically_normal()` the same PURELY LEXICAL way
// asset_ref.cpp's own `resolve()` does, never `std::filesystem::canonical`
// (nothing here needs to exist on disk to be named).
//
// Exit contract: Validation (3) iff the manifest is non-empty — the exact
// enumerated set for today's Warden corpus (scripts/verify.sh's "warden
// contract audit" step asserts it verbatim); Ok (0) once a later milestone
// completes the corpus (m4-warden-assets-complete's own exit test runs this
// SAME command expecting 0). A hard parser/schema error in a present file
// (there are none in this corpus) refuses with the loader's own error,
// never folded into the manifest.

#include "cli/verbs/validate_audit.h"

#include "core/base/file_io.h"
#include "core/loader/asset_ref.h"
#include "core/loader/component_vocab.h"
#include "core/loader/loader.h"
#include "core/loader/uid_registry.h"
#include "core/loader/yaml.h"
#include "core/reflect/builtin_events.h"
#include "core/reflect/registry.h"
#include "ts/toolchain/toolchain.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace midday::cli {
namespace {

namespace fs = std::filesystem;

VerbOutcome refuse(Error error) {
    VerbOutcome out;
    out.exit = Exit::Validation;
    out.error = std::move(error);
    return out;
}

std::string norm(const fs::path& path) {
    return path.lexically_normal().generic_string();
}

std::string rel_to_root(const std::string& path, const std::string& root) {
    return fs::path(path).lexically_relative(root).generic_string();
}

// `authored` as written inside `referencing_file`, resolved against that
// file's own directory (the corpus's authoring convention throughout), then
// re-expressed root-relative — the SAME two-step composition every missing
// reference in this audit goes through, whether it came from a loader Gap
// (referencing_file/authored = gap.file/gap.what) or a raw {uid,path} ref
// site this audit finds on its own (referencing_file/authored = the yaml
// file/the ref's path text).
std::string resolve_ref(const std::string& referencing_file,
                        const std::string& authored,
                        const std::string& root) {
    const std::string full = norm(fs::path(referencing_file).parent_path() / authored);
    return rel_to_root(full, root);
}

std::string_view classify_kind(std::string_view path) {
    if (path.ends_with(".entity.yaml"))
        return "prefab";
    if (path.ends_with(".model.yaml"))
        return "model";
    if (path.ends_with(".ts"))
        return "script";
    if (path.ends_with(".png"))
        return "golden";
    return "asset";
}

// See design decision #2 above: documented, spec-normative future NATIVE
// component vocabulary, an engine-roadmap gap rather than Warden-corpus
// completion debt.
bool is_engine_roadmap_component(std::string_view name) {
    return name == "MeshRenderer" || name == "Spline" || name == "VirtualCamera";
}

// See design decision #1 above: a narrow "identifier text after the
// decorator" lookup (ts/toolchain/driver.js's own decoratorCall() matches
// decorators by identifier text too, never the checker) — deliberately not
// `script extract`'s full schema walk, which this corpus's damage_on_touch.ts
// cannot clear today for a reason unrelated to whether the component exists.
std::optional<std::string> component_class_name(const std::string& source) {
    const std::size_t decorator = source.find("@component(");
    if (decorator == std::string::npos)
        return std::nullopt;
    const std::size_t class_kw = source.find("class ", decorator);
    if (class_kw == std::string::npos)
        return std::nullopt;
    std::size_t start = class_kw + 6; // strlen("class ")
    while (start < source.size() && std::isspace(static_cast<unsigned char>(source[start])) != 0)
        ++start;
    std::size_t end = start;
    while (end < source.size() &&
           (std::isalnum(static_cast<unsigned char>(source[end])) != 0 || source[end] == '_'))
        ++end;
    if (end == start)
        return std::nullopt;
    return source.substr(start, end - start);
}

// The one testkit call this corpus's tests/*.spec.ts makes:
// `toMatchGolden('goldens/warden_dead.png', ...)`. `midday/testkit` is not
// implemented yet (later milestone) — this reads the literal argument off
// already-authored source text, never a TS walk, scoped to exactly this one
// call the testing pillar promises.
std::optional<std::string> golden_literal(const std::string& source) {
    constexpr std::string_view kMarker = "toMatchGolden(";
    const std::size_t call = source.find(kMarker);
    if (call == std::string::npos)
        return std::nullopt;
    std::size_t i = call + kMarker.size();
    while (i < source.size() && std::isspace(static_cast<unsigned char>(source[i])) != 0)
        ++i;
    if (i >= source.size() || (source[i] != '\'' && source[i] != '"'))
        return std::nullopt;
    const char quote = source[i++];
    const std::size_t start = i;
    while (i < source.size() && source[i] != quote)
        ++i;
    if (i >= source.size())
        return std::nullopt;
    return source.substr(start, i - start);
}

struct MissingFile {
    std::string kind;
    std::string path;
    std::string referenced_from;
};

struct MissingComponent {
    std::string name;
    std::string referenced_from;
};

struct OutOfScopeGap {
    std::string kind;
    std::string what;
    std::string referenced_from;
    std::string detail;
};

void add_missing_file(std::vector<MissingFile>& files, MissingFile file) {
    for (const MissingFile& existing : files)
        if (existing.path == file.path)
            return;
    files.push_back(std::move(file));
}

void add_missing_component(std::vector<MissingComponent>& components, MissingComponent component) {
    for (const MissingComponent& existing : components)
        if (existing.name == component.name)
            return;
    components.push_back(std::move(component));
}

// Design decisions #2/#3: sorts one raw loader::Gap into the missing
// manifest or the out-of-scope transparency bucket.
void classify_gap(const loader::Gap& gap,
                  const std::string& root,
                  std::vector<MissingFile>& files,
                  std::vector<MissingComponent>& components,
                  std::vector<OutOfScopeGap>& out_of_scope) {
    const std::string referenced_from = rel_to_root(norm(fs::path(gap.file)), root);
    if (gap.kind == "prefab" || gap.kind == "entity" || gap.kind == "script") {
        const std::string path = resolve_ref(gap.file, gap.what, root);
        add_missing_file(files,
                         MissingFile{.kind = std::string(classify_kind(path)),
                                     .path = path,
                                     .referenced_from = referenced_from});
        return;
    }
    if (gap.kind == "component") {
        if (is_engine_roadmap_component(gap.what)) {
            out_of_scope.push_back(OutOfScopeGap{.kind = gap.kind,
                                                 .what = gap.what,
                                                 .referenced_from = referenced_from,
                                                 .detail = gap.detail});
            return;
        }
        add_missing_component(
            components, MissingComponent{.name = gap.what, .referenced_from = referenced_from});
        return;
    }
    // "statechart" (a native engine-capability gap) or any future kind:
    // never a content-completion item.
    out_of_scope.push_back(OutOfScopeGap{.kind = gap.kind,
                                         .what = gap.what,
                                         .referenced_from = referenced_from,
                                         .detail = gap.detail});
}

Json missing_file_json(const MissingFile& file) {
    Json out = Json::object();
    out.set("kind", file.kind);
    out.set("path", file.path);
    out.set("referenced_from", file.referenced_from);
    return out;
}

Json missing_component_json(const MissingComponent& component) {
    Json out = Json::object();
    out.set("name", component.name);
    out.set("referenced_from", component.referenced_from);
    return out;
}

Json out_of_scope_json(const OutOfScopeGap& gap) {
    Json out = Json::object();
    out.set("kind", gap.kind);
    out.set("what", gap.what);
    out.set("referenced_from", gap.referenced_from);
    out.set("detail", gap.detail);
    return out;
}

} // namespace

VerbOutcome run_audit_missing(const std::string& root_arg) {
    std::error_code ec;
    if (!fs::is_directory(root_arg, ec))
        return refuse(Error{.code = "loader.io",
                            .message = root_arg + ": --audit-missing needs a project directory"});
    const std::string root = norm(fs::path(root_arg));

    // ---- present TS components -> the vocab (design decision #1) ---------
    // `present_scripts` is transparency only (no exit test reads it): every
    // *.ts this audit KNOWS is present, whether or not it validates one —
    // components/ is checked+named here; states/*.ts and tests/*.spec.ts are
    // listed further below without re-validating them (out of this audit's
    // scope: they parse against future engine surface — Animator, testkit —
    // that does not exist yet, design decision #1's own rationale).
    loader::ComponentVocab vocab;
    Json present_scripts = Json::array();
    const std::string components_dir = root + "/components";
    if (fs::is_directory(components_dir, ec)) {
        script::Toolchain toolchain;
        for (const std::string& ts_path : loader::find_files_with_suffix(components_dir, ".ts")) {
            script::CheckOutcome checked = toolchain.check(ts_path);
            if (checked.failure.has_value()) {
                VerbOutcome out;
                out.exit = Exit::Failure;
                out.error = std::move(checked.failure);
                return out;
            }
            if (!checked.ok) {
                VerbOutcome out;
                out.exit = Exit::Validation;
                Error error{.code = "script.type_error",
                            .message = checked.diagnostics.front().to_string()};
                error.details.set("file", ts_path);
                out.error = std::move(error);
                return out;
            }
            base::ReadFileResult source = base::read_file(ts_path, "loader.io");
            if (source.error.has_value())
                return refuse(std::move(*source.error));
            if (std::optional<std::string> name = component_class_name(source.bytes))
                vocab.extracted.push_back(*name);
            present_scripts.push(rel_to_root(ts_path, root));
        }
    }
    const std::string states_dir = root + "/states";
    if (fs::is_directory(states_dir, ec))
        for (const std::string& ts_path : loader::find_files_with_suffix(states_dir, ".ts"))
            present_scripts.push(rel_to_root(ts_path, root));

    std::vector<MissingFile> missing_files;
    std::vector<MissingComponent> missing_components;
    std::vector<OutOfScopeGap> out_of_scope;
    Json present_files = Json::array();

    // ---- the scene-rooted lenient load: the canonical raw-gap source ------
    // (mirrors `scene print --full`'s own entry point, cli/verbs/scene.cpp)
    // — the ONLY path that resolves the scene's own `events:` declarations
    // before recursing into its prefabs/machines, so a standalone entity/
    // machine file's false "event not declared" noise never appears here.
    for (const std::string& scene_path : loader::find_files_with_suffix(root, ".scene.yaml")) {
        reflect::Registry registry;
        reflect::register_builtin_events(registry);
        loader::SceneLoadResult loaded =
            loader::load_scene(scene_path, registry, /*lenient=*/true, vocab);
        if (loaded.error.has_value())
            return refuse(std::move(*loaded.error));
        if (!loaded.scene.has_value())
            return refuse(Error{.code = "loader.bad_ref", .message = "scene load failed"});
        present_files.push(rel_to_root(scene_path, root));
        for (const loader::Gap& gap : loaded.scene->gaps)
            classify_gap(gap, root, missing_files, missing_components, out_of_scope);
    }

    // ---- supplementary {uid?, path} scan (design decision #3) -------------
    // Every *.yaml under root, raw-parsed (proving present-file syntax) and
    // walked for asset-ref shapes core/loader/asset_ref.h already knows
    // (`midday check`'s own mechanism) — the only way to see a ref nested
    // inside a GENERIC component's opaque fields (MeshRenderer.model).
    for (const std::string& yaml_path : loader::find_files_with_suffix(root, ".yaml")) {
        loader::YamlParseResult parsed = loader::parse_yaml_file(yaml_path);
        if (parsed.error.has_value())
            return refuse(std::move(*parsed.error));
        const std::string yaml_rel = rel_to_root(yaml_path, root);
        bool already_listed = false;
        for (const Json& listed : present_files.elements())
            already_listed = already_listed || listed.as_string() == yaml_rel;
        if (!already_listed)
            present_files.push(yaml_rel);
        for (loader::RefSite& site : loader::find_asset_refs(parsed.root)) {
            const std::string path = resolve_ref(yaml_path, site.fields.path->scalar, root);
            const std::string full = norm(fs::path(root) / path);
            if (fs::is_regular_file(full, ec))
                continue;
            add_missing_file(missing_files,
                             MissingFile{.kind = std::string(classify_kind(path)),
                                         .path = path,
                                         .referenced_from = yaml_rel});
        }
    }

    // ---- golden literal scan ------------------------------------------------
    const std::string tests_dir = root + "/tests";
    if (fs::is_directory(tests_dir, ec)) {
        for (const std::string& spec_path : loader::find_files_with_suffix(tests_dir, ".spec.ts")) {
            base::ReadFileResult source = base::read_file(spec_path, "loader.io");
            if (source.error.has_value())
                return refuse(std::move(*source.error));
            present_scripts.push(rel_to_root(spec_path, root));
            std::optional<std::string> literal = golden_literal(source.bytes);
            if (!literal.has_value())
                continue;
            const std::string full = norm(fs::path(root) / *literal);
            if (fs::is_regular_file(full, ec))
                continue;
            add_missing_file(missing_files,
                             MissingFile{.kind = "golden",
                                         .path = rel_to_root(full, root),
                                         .referenced_from = rel_to_root(spec_path, root)});
        }
    }

    // ---- the Animator rig-wiring note (Aurora D-19) ------------------------
    constexpr std::string_view kBodyModel = "models/warden_body.model.yaml";
    const bool body_missing = std::ranges::any_of(
        missing_files, [kBodyModel](const MissingFile& file) { return file.path == kBodyModel; });
    Json wiring = Json::array();
    if (body_missing) {
        Json note = Json::object();
        note.set("kind", "animator_rig");
        note.set("implied_by", std::string(kBodyModel));
        note.set("detail",
                 "warden_body.model.yaml (Warden's MeshRenderer target, missing) implies a "
                 "mesh+skeleton rig; states/slash_attack.ts already assumes an Animator "
                 "component ('midday' exports no such member yet) to drive it -- the "
                 "model-to-animation binding Aurora D-19 calls out is not wired yet.");
        wiring.push(std::move(note));
    }

    std::ranges::sort(missing_files, {}, &MissingFile::path);
    std::ranges::sort(missing_components, {}, &MissingComponent::name);

    Json files_json = Json::array();
    for (const MissingFile& file : missing_files)
        files_json.push(missing_file_json(file));
    Json components_json = Json::array();
    for (const MissingComponent& component : missing_components)
        components_json.push(missing_component_json(component));
    Json out_of_scope_array = Json::array();
    for (const OutOfScopeGap& gap : out_of_scope)
        out_of_scope_array.push(out_of_scope_json(gap));

    // Captured before the moves below: `wiring` (a Json array) is sunk into
    // `missing` a few lines down, so its own .elements().size() would read a
    // moved-from value by the time the human string is built.
    const std::size_t wiring_count = wiring.elements().size();
    const auto total = static_cast<std::int64_t>(missing_files.size()) +
                       static_cast<std::int64_t>(missing_components.size()) +
                       static_cast<std::int64_t>(wiring_count);

    VerbOutcome out;
    out.payload.set("root", root);
    out.payload.set("present_files", std::move(present_files));
    out.payload.set("present_scripts", std::move(present_scripts));
    Json missing = Json::object();
    missing.set("files", std::move(files_json));
    missing.set("components", std::move(components_json));
    missing.set("wiring", std::move(wiring));
    out.payload.set("missing", std::move(missing));
    out.payload.set("missing_count", total);
    out.payload.set("out_of_scope", std::move(out_of_scope_array));

    if (total > 0) {
        out.exit = Exit::Validation;
        out.error = Error{.code = "validate.audit_missing",
                          .message = root + ": " + std::to_string(total) +
                                     " known-missing reference(s) in the completion manifest"};
    }
    out.human = root + ": " + std::to_string(total) + " known-missing reference(s) (" +
                std::to_string(missing_files.size()) + " file(s), " +
                std::to_string(missing_components.size()) + " component(s), " +
                std::to_string(wiring_count) + " wiring note(s))";
    return out;
}

} // namespace midday::cli
