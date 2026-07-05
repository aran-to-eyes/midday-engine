// core/loader/machine_load.cpp — `*.machine.yaml` structure: file header,
// vars, regions, states/substates, script refs, children under states, and
// the machine-wide reference validation (goto/initial targets region-wide,
// event names against the vocabulary closure, script files on disk). Pairs
// and dope sheets parse in machine_parts.cpp; shared context in
// machine_ctx.h. Statechart::instantiate re-validates the finished desc
// (defense in depth) — an author should never see those diagnostics,
// because every refusal here carries file:line:col.

#include "core/expr/value.h"
#include "core/loader/machine_ctx.h"
#include "core/loader/parse_util.h"

#include <array>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace midday::loader {

using detail::err_at;
using detail::err_node;
using detail::MachineCtx;
using detail::Parsed;
using detail::RegionCtx;

namespace {

void parse_script(MachineCtx& ctx, RegionCtx& region, base::Name state, const YamlNode& node) {
    Parsed<std::string> ref = detail::get_name(node, ctx.path);
    if (ref.error.has_value()) {
        ctx.fail(std::move(*ref.error));
        return;
    }
    StateScriptRef script;
    script.region = region.region;
    script.state = state;
    script.ref = ref.value;
    script.line = node.line;
    script.path = (std::filesystem::path(ctx.root_dir) / ref.value).generic_string();
    if (!std::filesystem::exists(script.path)) {
        // Lenient mode: the script seat is skipped entirely (nothing to bind
        // at run time) and the gap is reported instead of refused.
        ctx.gap_or_fail(
            "script",
            ref.value,
            node.line,
            node.col,
            "state script '" + ref.value + "' not found (resolved: " + script.path + ")",
            err_node("loader.bad_ref",
                     ctx.path,
                     node,
                     "state script '" + ref.value + "' not found (resolved: " + script.path +
                         "; script paths are project-root-relative)"));
        return;
    }
    ctx.out.scripts.push_back(std::move(script));
}

statechart::StateDesc parse_state(MachineCtx& ctx, RegionCtx& region, const YamlEntry& entry) {
    statechart::StateDesc state;
    state.name = base::Name(entry.key);
    if (entry.key.empty()) {
        ctx.fail(err_at(
            "loader.bad_value", ctx.path, entry.key_line, entry.key_col, "empty state name"));
        return state;
    }
    for (const std::string& existing : region.state_names) {
        if (existing == entry.key) {
            ctx.fail(err_at("loader.duplicate",
                            ctx.path,
                            entry.key_line,
                            entry.key_col,
                            "state '" + entry.key + "' is already declared in region '" +
                                std::string(region.region.view()) +
                                "' (state names are region-unique)"));
            return state;
        }
    }
    region.state_names.push_back(entry.key);
    ctx.derive(entry.key + ".finished"); // the per-state event family (spec 4.1)

    const YamlNode& body = entry.node();
    if (body.is_null())
        return state; // "State: {}" and bare "State:" are both empty states
    if (!body.is_map()) {
        ctx.fail(err_node("loader.bad_value", ctx.path, body, "expected a state mapping"));
        return state;
    }
    static constexpr std::array<std::string_view, 9> kAllowed = {"script",
                                                                 "on",
                                                                 "Transition",
                                                                 "components",
                                                                 "states",
                                                                 "initial",
                                                                 "history",
                                                                 "sequence",
                                                                 "children"};
    if (auto error = detail::check_keys(body, ctx.path, kAllowed)) {
        ctx.fail(std::move(*error));
        return state;
    }

    if (const YamlNode* history = body.find("history")) {
        Parsed<bool> flag = detail::get_bool(*history, ctx.path);
        if (flag.error.has_value()) {
            ctx.fail(std::move(*flag.error));
            return state;
        }
        state.history = flag.value;
    }
    if (const YamlNode* script = body.find("script"))
        parse_script(ctx, region, state.name, *script);
    // `on:` is authoring sugar for a Transition component pair list (spec
    // 4.2); `Transition:` is that CANONICAL, already-desugared spelling
    // (`scene print --full` emits it, never `on:` — SPEC-GAP #5). Both
    // parse through the identical pair-list grammar into state.transitions;
    // carrying both on the same state is ambiguous, so it refuses.
    const YamlNode* on = body.find("on");
    const YamlNode* canonical_transition = body.find("Transition");
    if (on != nullptr && canonical_transition != nullptr) {
        ctx.fail(err_node("loader.bad_value",
                          ctx.path,
                          *canonical_transition,
                          "state '" + entry.key +
                              "' carries both 'on:' and 'Transition:' (Transition: is on:'s "
                              "canonical form; author exactly one)"));
        return state;
    }
    if (const YamlNode* pairs = on != nullptr ? on : canonical_transition)
        state.transitions = detail::parse_pair_list(ctx, region, *pairs, entry.key);
    if (ctx.failed())
        return state;

    if (const YamlNode* components = body.find("components")) {
        state.components = detail::parse_state_components(ctx, *components);
        if (ctx.failed())
            return state;
    }

    if (const YamlNode* sequence = body.find("sequence")) {
        detail::SequenceParse sheet = detail::parse_sequence(ctx, region, *sequence);
        if (ctx.failed())
            return state;
        if (sheet.then_target.has_value()) {
            // `then: S` == an ordinary pair on the state's finished event,
            // appended after the authored pairs (D-BUILD-057 sugar rule).
            statechart::TransitionDesc chained;
            chained.event = base::Name(entry.key + ".finished");
            chained.target = base::Name(*sheet.then_target);
            state.transitions.push_back(chained);
        }
        state.sequence = std::move(sheet.sheet);
    }
    if (const YamlNode* children = body.find("children"))
        detail::parse_children(ctx, region, state.name, *children);
    if (ctx.failed())
        return state;

    if (const YamlNode* substates = body.find("states")) {
        if (!substates->is_map() || substates->map.empty()) {
            ctx.fail(err_node(
                "loader.bad_value", ctx.path, *substates, "expected a non-empty state mapping"));
            return state;
        }
        for (const YamlEntry& substate : substates->map) {
            state.substates.push_back(parse_state(ctx, region, substate));
            if (ctx.failed())
                return state;
        }
        const YamlNode* initial_field = body.find("initial");
        if (initial_field == nullptr) {
            // NATIVE span-scoped substate activation (a state whose ONLY
            // substates are span targets, entered/exited by the span's own
            // open/close, never a normal `initial:` descent) is m4 work
            // (boss.machine.yaml's header comment: "m4 binds span-scoped
            // activation natively; this file then sheds the two [manual
            // open/close] pairs") — examples/warden/brains/warden.machine.yaml
            // already authors that shape. Lenient mode reports the gap and
            // picks the first substate as a syntactically-valid placeholder
            // so the REST of the tree still parses and prints; every other
            // caller (default: strict) keeps the exact M0 hard refusal.
            ctx.gap_or_fail("statechart",
                            std::string(state.name.view()) + ".initial",
                            entry.key_line,
                            entry.key_col,
                            "state '" + std::string(state.name.view()) +
                                "' has substates but no 'initial:' (native span-scoped substate "
                                "activation is not implemented yet)",
                            err_node("loader.bad_value",
                                     ctx.path,
                                     body,
                                     "a state with substates requires 'initial'"));
            if (ctx.failed())
                return state;
            state.initial = state.substates.front().name;
            return state;
        }
        Parsed<std::string> initial_name = detail::get_name(*initial_field, ctx.path);
        if (initial_name.error.has_value()) {
            ctx.fail(std::move(*initial_name.error));
            return state;
        }
        bool direct = false;
        for (const statechart::StateDesc& substate : state.substates)
            direct = direct || substate.name.view() == initial_name.value;
        if (!direct) {
            ctx.fail(err_node("loader.bad_ref",
                              ctx.path,
                              *initial_field,
                              "initial '" + initial_name.value + "' must name a direct substate"));
            return state;
        }
        state.initial = base::Name(initial_name.value);
    } else if (body.find("initial") != nullptr) {
        ctx.fail(err_node(
            "loader.bad_value", ctx.path, *body.find("initial"), "initial requires substates"));
        return state;
    }
    return state;
}

void parse_region(MachineCtx& ctx, const YamlEntry& entry) {
    RegionCtx region;
    region.region = base::Name(entry.key);
    statechart::RegionDesc desc;
    desc.name = region.region;
    if (entry.key.empty()) {
        ctx.fail(err_at(
            "loader.bad_value", ctx.path, entry.key_line, entry.key_col, "empty region name"));
        return;
    }
    const YamlNode& body = entry.node();
    if (!body.is_map()) {
        ctx.fail(err_node("loader.bad_value", ctx.path, body, "expected a region mapping"));
        return;
    }
    static constexpr std::array<std::string_view, 4> kAllowed = {
        "initial", "history", "anystate", "states"};
    if (auto error = detail::check_keys(body, ctx.path, kAllowed)) {
        ctx.fail(std::move(*error));
        return;
    }
    if (const YamlNode* history = body.find("history")) {
        Parsed<bool> flag = detail::get_bool(*history, ctx.path);
        if (flag.error.has_value()) {
            ctx.fail(std::move(*flag.error));
            return;
        }
        desc.history = flag.value;
    }
    if (const YamlNode* anystate = body.find("anystate")) {
        desc.any_state = detail::parse_pair_list(ctx, region, *anystate, {});
        if (ctx.failed())
            return;
    }

    detail::FieldResult states = detail::require_field(body, ctx.path, "states", "a region");
    if (states.error.has_value()) {
        ctx.fail(std::move(*states.error));
        return;
    }
    if (!states.node->is_map() || states.node->map.empty()) {
        ctx.fail(err_node(
            "loader.bad_value", ctx.path, *states.node, "expected a non-empty state mapping"));
        return;
    }
    for (const YamlEntry& state : states.node->map) {
        desc.states.push_back(parse_state(ctx, region, state));
        if (ctx.failed())
            return;
    }

    detail::FieldResult initial = detail::require_field(body, ctx.path, "initial", "a region");
    if (initial.error.has_value()) {
        ctx.fail(std::move(*initial.error));
        return;
    }
    Parsed<std::string> initial_name = detail::get_name(*initial.node, ctx.path);
    if (initial_name.error.has_value()) {
        ctx.fail(std::move(*initial_name.error));
        return;
    }
    bool top_level = false;
    for (const statechart::StateDesc& state : desc.states)
        top_level = top_level || state.name.view() == initial_name.value;
    if (!top_level) {
        ctx.fail(err_node("loader.bad_ref",
                          ctx.path,
                          *initial.node,
                          "initial '" + initial_name.value +
                              "' must name a top-level state of "
                              "region '" +
                              entry.key + "'"));
        return;
    }
    desc.initial = base::Name(initial_name.value);

    // goto/then targets resolve REGION-WIDE by name (machine_desc.h rule).
    for (const detail::TargetRef& target : region.targets) {
        bool known = false;
        for (const std::string& name : region.state_names)
            known = known || name == target.target;
        if (!known) {
            ctx.fail(err_at("loader.bad_ref",
                            ctx.path,
                            target.line,
                            target.col,
                            "goto target '" + target.target + "' is not a state of region '" +
                                entry.key + "'"));
            return;
        }
    }
    ctx.out.desc.regions.push_back(std::move(desc));
}

void parse_vars(MachineCtx& ctx, const YamlNode& node) {
    if (!node.is_map()) {
        ctx.fail(err_node("loader.bad_value", ctx.path, node, "expected a {name: type} mapping"));
        return;
    }
    for (const YamlEntry& var : node.map) {
        if (var.key.empty()) {
            ctx.fail(
                err_at("loader.bad_value", ctx.path, var.key_line, var.key_col, "empty var name"));
            return;
        }
        if (ctx.env.find(var.key) >= 0) {
            ctx.fail(err_at("loader.duplicate",
                            ctx.path,
                            var.key_line,
                            var.key_col,
                            "var '" + var.key + "' is already declared"));
            return;
        }
        Parsed<std::string> spelling = detail::get_name(var.node(), ctx.path);
        if (spelling.error.has_value()) {
            ctx.fail(std::move(*spelling.error));
            return;
        }
        std::optional<expr::ValueType> type;
        for (std::size_t i = 0; i < expr::kValueTypeCount; ++i) {
            const auto candidate = static_cast<expr::ValueType>(i);
            if (expr::to_string(candidate) == spelling.value)
                type = candidate;
        }
        if (!type.has_value()) {
            ctx.fail(err_node("loader.bad_value",
                              ctx.path,
                              var.node(),
                              "unknown var type '" + spelling.value +
                                  "' (expression types: bool, int, float, string, name, vec2, "
                                  "vec3, vec4, quat)"));
            return;
        }
        ctx.env.declare(var.key, *type);
        ctx.out.desc.vars.push_back(statechart::VarDesc{var.key, *type});
    }
}

} // namespace

MachineLoadResult load_machine_file(const std::string& path,
                                    const std::string& root_dir,
                                    const reflect::Registry& registry,
                                    const EventsDecl& vocab,
                                    const ComponentVocab& components_vocab,
                                    bool lenient) {
    MachineLoadResult result;
    YamlParseResult parsed = parse_yaml_file(path);
    if (parsed.error.has_value()) {
        result.error = std::move(parsed.error);
        return result;
    }
    const YamlNode& root = parsed.root;
    if (auto error = detail::check_format(root, path, "machine")) {
        result.error = std::move(error);
        return result;
    }
    static constexpr std::array<std::string_view, 4> kAllowed = {
        "format", "machine", "vars", "regions"};
    if (auto error = detail::check_keys(root, path, kAllowed)) {
        result.error = std::move(error);
        return result;
    }

    MachineCtx ctx{.path = path,
                   .root_dir = root_dir,
                   .registry = registry,
                   .vocab = vocab,
                   .components_vocab = components_vocab,
                   .lenient = lenient};
    ctx.out.path = path;

    detail::FieldResult name = detail::require_field(root, path, "machine", "a machine file");
    if (name.error.has_value()) {
        result.error = std::move(name.error);
        return result;
    }
    Parsed<std::string> machine_name = detail::get_name(*name.node, path);
    if (machine_name.error.has_value()) {
        result.error = std::move(machine_name.error);
        return result;
    }
    ctx.out.desc.name = base::Name(machine_name.value);

    if (const YamlNode* vars = root.find("vars")) {
        parse_vars(ctx, *vars);
        if (ctx.failed()) {
            result.error = std::move(ctx.error);
            return result;
        }
    }

    detail::FieldResult regions = detail::require_field(root, path, "regions", "a machine file");
    if (regions.error.has_value()) {
        result.error = std::move(regions.error);
        return result;
    }
    if (!regions.node->is_map() || regions.node->map.empty()) {
        result.error = err_node(
            "loader.bad_value", path, *regions.node, "expected a non-empty region mapping");
        return result;
    }
    for (const YamlEntry& region : regions.node->map) {
        for (const statechart::RegionDesc& existing : ctx.out.desc.regions) {
            if (existing.name.view() == region.key) {
                result.error = err_at("loader.duplicate",
                                      path,
                                      region.key_line,
                                      region.key_col,
                                      "region '" + region.key + "' is already declared");
                return result;
            }
        }
        parse_region(ctx, region);
        if (ctx.failed()) {
            result.error = std::move(ctx.error);
            return result;
        }
    }

    // The vocabulary closure: every referenced event must be declared (an
    // events file), built-in (reflect vocabulary), or derived from this
    // machine ("<state>.finished", "<span>.opened"/".closed"). A machine
    // file's vocabulary comes from its SCENE's `events:` list (there is no
    // per-machine equivalent) — printing one standalone (`midday scene
    // print` on a bare `*.machine.yaml`) walks its own directory instead
    // (loader_yaml.md's project-root convention) and may legitimately miss
    // events declared in a sibling directory. Lenient mode reports the gap
    // and keeps parsing rather than refusing a file that is only
    // incompletely OBSERVABLE from where it happens to sit, not actually
    // wrong; every other caller (default: strict — `midday run` always
    // loads a machine THROUGH its scene, which supplies the real vocab)
    // keeps the exact M0 hard refusal.
    for (const detail::EventUse& use : ctx.uses) {
        if (vocab.has_event(use.event))
            continue;
        if (registry.find_event(base::Name(use.event)) != nullptr)
            continue;
        bool derived = false;
        for (const std::string& candidate : ctx.derived)
            derived = derived || candidate == use.event;
        if (derived)
            continue;
        ctx.gap_or_fail("event",
                        use.event,
                        use.line,
                        use.col,
                        "event '" + use.event +
                            "' is not declared in the vocabulary visible "
                            "from here",
                        err_at("loader.bad_ref",
                               path,
                               use.line,
                               use.col,
                               "unknown event '" + use.event +
                                   "' (declare it in an events file; built-in and derived "
                                   "<state>.finished / <span>.opened|.closed names pass)"));
        if (ctx.failed()) {
            result.error = std::move(ctx.error);
            return result;
        }
    }

    result.machine = std::move(ctx.out);
    return result;
}

} // namespace midday::loader
