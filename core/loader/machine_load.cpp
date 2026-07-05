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

void parse_children(MachineCtx& ctx, RegionCtx& region, base::Name state, const YamlNode& node) {
    if (!node.is_seq()) {
        ctx.fail(err_node(
            "loader.bad_value", ctx.path, node, "expected a list of {entity[, at]} children"));
        return;
    }
    StateChildren children;
    children.region = region.region;
    children.state = state;
    for (const YamlNode& child : node.seq) {
        if (!child.is_map()) {
            ctx.fail(
                err_node("loader.bad_value", ctx.path, child, "expected an {entity[, at]} map"));
            return;
        }
        static constexpr std::array<std::string_view, 2> kAllowed = {"entity", "at"};
        if (auto error = detail::check_keys(child, ctx.path, kAllowed)) {
            ctx.fail(std::move(*error));
            return;
        }
        detail::FieldResult entity =
            detail::require_field(child, ctx.path, "entity", "a state child");
        if (entity.error.has_value()) {
            ctx.fail(std::move(*entity.error));
            return;
        }
        Parsed<std::string> name = detail::get_name(*entity.node, ctx.path);
        if (name.error.has_value()) {
            ctx.fail(std::move(*name.error));
            return;
        }
        StateChildDesc desc;
        desc.entity = base::Name(name.value);
        for (const StateChildDesc& existing : children.children) {
            if (existing.entity == desc.entity) {
                ctx.fail(err_node("loader.duplicate",
                                  ctx.path,
                                  *entity.node,
                                  "child entity '" + name.value + "' is already declared"));
                return;
            }
        }
        if (const YamlNode* at = child.find("at")) {
            Parsed<math::Vec3> translation = detail::get_vec3(*at, ctx.path);
            if (translation.error.has_value()) {
                ctx.fail(std::move(*translation.error));
                return;
            }
            desc.at.translation = translation.value;
        }
        children.children.push_back(desc);
    }
    ctx.out.children.push_back(std::move(children));
}

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
    script.path = (std::filesystem::path(ctx.root_dir) / ref.value).generic_string();
    if (!std::filesystem::exists(script.path)) {
        ctx.fail(err_node("loader.bad_ref",
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
    static constexpr std::array<std::string_view, 7> kAllowed = {
        "script", "on", "states", "initial", "history", "sequence", "children"};
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
    if (const YamlNode* on = body.find("on"))
        state.transitions = detail::parse_pair_list(ctx, region, *on, entry.key);
    if (ctx.failed())
        return state;

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
        parse_children(ctx, region, state.name, *children);
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
        detail::FieldResult initial =
            detail::require_field(body, ctx.path, "initial", "a state with substates");
        if (initial.error.has_value()) {
            ctx.fail(std::move(*initial.error));
            return state;
        }
        Parsed<std::string> initial_name = detail::get_name(*initial.node, ctx.path);
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
                              *initial.node,
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
                                    const EventsDecl& vocab) {
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

    MachineCtx ctx{.path = path, .root_dir = root_dir, .vocab = vocab};
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
    // machine ("<state>.finished", "<span>.opened"/".closed").
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
        result.error = err_at("loader.bad_ref",
                              path,
                              use.line,
                              use.col,
                              "unknown event '" + use.event +
                                  "' (declare it in an events file; built-in and derived "
                                  "<state>.finished / <span>.opened|.closed names pass)");
        return result;
    }

    result.machine = std::move(ctx.out);
    return result;
}

} // namespace midday::loader
