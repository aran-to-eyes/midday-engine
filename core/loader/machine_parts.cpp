// core/loader/machine_parts.cpp — transition pairs and dope sheets of the
// machine-file loader (structure lives in machine_load.cpp; shared context
// in machine_ctx.h).
//
// Canonicalizations here (D-BUILD-057/077): `on:` pairs -> TransitionDesc
// in declaration order (the A.2 tie-break), `self.finished` -> the owning
// state's per-state event, pair `key:` values -> machine channels, span
// names -> derived "<span>.opened"/".closed" vocabulary.

#include "core/expr/expr.h"
#include "core/loader/machine_ctx.h"
#include "core/loader/parse_util.h"

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace midday::loader::detail {

void MachineCtx::add_channel(std::string_view name) {
    const base::Name channel{name};
    for (const base::Name& existing : out.desc.channels)
        if (existing == channel)
            return;
    out.desc.channels.push_back(channel);
}

namespace {

// The spec 4.2 symbolic key vocabulary (self/root/global/declared group),
// shared by a transition pair's `key:` (a SUBSCRIPTION: `global`/a group
// widens MachineDesc::channels, spec's existing behavior) and a sequence
// trigger keyframe's `key:` (m1-scene-format's "symbolic keys" deliverable,
// examples/warden/brains/warden.machine.yaml's `key: self` trigger — an
// EMIT-side channel; `self`/`root` already match the runtime's existing
// "always the host channel" behavior exactly, core/statechart/sequences.cpp;
// routing an EMIT to `global`/a group is deferred — SCOPE, not this node's
// runtime to extend — so those spellings validate and register the same
// SUBSCRIPTION channel a listening pair would, a harmless imprecision this
// node accepts rather than leaving the grammar unparseable).
void validate_key(MachineCtx& ctx, const YamlNode& key_node) {
    Parsed<std::string> spelling = get_name(key_node, ctx.path);
    if (spelling.error.has_value()) {
        ctx.fail(std::move(*spelling.error));
        return;
    }
    if (spelling.value == "global") {
        ctx.add_channel("global");
    } else if (ctx.vocab.has_group(spelling.value)) {
        ctx.add_channel(spelling.value);
    } else if (spelling.value != "self" && spelling.value != "root") {
        ctx.fail(err_node("loader.bad_ref",
                          ctx.path,
                          key_node,
                          "unknown key '" + spelling.value +
                              "' (self, root, global, or a group declared in an events "
                              "file's keys: list)"));
    }
}

statechart::TransitionDesc
parse_pair(MachineCtx& ctx, RegionCtx& region, const YamlNode& node, std::string_view owner_state) {
    statechart::TransitionDesc pair;
    if (!node.is_map()) {
        ctx.fail(err_node("loader.bad_value",
                          ctx.path,
                          node,
                          "expected a {event, goto[, key, priority, if]} pair"));
        return pair;
    }
    static constexpr std::array<std::string_view, 5> kAllowed = {
        "event", "key", "goto", "priority", "if"};
    if (auto error = check_keys(node, ctx.path, kAllowed)) {
        ctx.fail(std::move(*error));
        return pair;
    }

    FieldResult event = require_field(node, ctx.path, "event", "a transition pair");
    FieldResult target = require_field(node, ctx.path, "goto", "a transition pair");
    if (event.error.has_value() || target.error.has_value()) {
        ctx.fail(event.error.has_value() ? std::move(*event.error) : std::move(*target.error));
        return pair;
    }
    Parsed<std::string> event_name = get_name(*event.node, ctx.path);
    Parsed<std::string> target_name = get_name(*target.node, ctx.path);
    if (event_name.error.has_value() || target_name.error.has_value()) {
        ctx.fail(event_name.error.has_value() ? std::move(*event_name.error)
                                              : std::move(*target_name.error));
        return pair;
    }

    // `self.finished` canonicalizes to the owning state's per-state event
    // (spec 4.1 spells "<state>.finished"; `self.` is authoring sugar).
    if (event_name.value == "self.finished") {
        if (owner_state.empty()) {
            ctx.fail(err_node("loader.bad_value",
                              ctx.path,
                              *event.node,
                              "self.finished is only valid on a state's own pairs"));
            return pair;
        }
        event_name.value = std::string(owner_state) + ".finished";
    } else {
        ctx.uses.push_back(EventUse{event_name.value, event.node->line, event.node->col});
    }
    pair.event = base::Name(event_name.value);
    pair.target = base::Name(target_name.value);
    region.targets.push_back(TargetRef{target_name.value, target.node->line, target.node->col});

    if (const YamlNode* key = node.find("key")) {
        validate_key(ctx, *key);
        if (ctx.failed())
            return pair;
    }
    if (const YamlNode* priority = node.find("priority")) {
        Parsed<std::int64_t> value = get_int(*priority, ctx.path);
        if (value.error.has_value()) {
            ctx.fail(std::move(*value.error));
            return pair;
        }
        if (value.value < std::numeric_limits<std::int32_t>::min() ||
            value.value > std::numeric_limits<std::int32_t>::max()) {
            ctx.fail(err_node(
                "loader.bad_value", ctx.path, *priority, "priority is out of int32 range"));
            return pair;
        }
        pair.priority = static_cast<std::int32_t>(value.value);
    }
    if (const YamlNode* filter = node.find("if")) {
        Parsed<std::string> source = get_name(*filter, ctx.path);
        if (source.error.has_value()) {
            ctx.fail(std::move(*source.error));
            return pair;
        }
        const expr::CompileResult compiled = expr::compile(source.value, ctx.env, ctx.path);
        if (compiled.diag.has_value()) {
            const expr::Diag& diag = *compiled.diag;
            base::Error error = err_node("loader.bad_value",
                                         ctx.path,
                                         *filter,
                                         "if: filter does not compile: " + diag.message);
            error.details.set("expr", diag.to_error().to_json());
            ctx.fail(std::move(error));
            return pair;
        }
        pair.condition = std::move(source.value);
    }
    return pair;
}

void parse_trigger_track(MachineCtx& ctx, const YamlNode& node, statechart::SequenceDesc& sheet) {
    if (!node.is_seq()) {
        ctx.fail(err_node("loader.bad_value",
                          ctx.path,
                          node,
                          "expected a list of {t, event[, payload]} keyframes"));
        return;
    }
    for (const YamlNode& keyframe : node.seq) {
        if (!keyframe.is_map()) {
            ctx.fail(err_node(
                "loader.bad_value", ctx.path, keyframe, "expected a {t, event[, payload]} map"));
            return;
        }
        static constexpr std::array<std::string_view, 4> kAllowed = {
            "t", "event", "key", "payload"};
        if (auto error = check_keys(keyframe, ctx.path, kAllowed)) {
            ctx.fail(std::move(*error));
            return;
        }
        FieldResult time = require_field(keyframe, ctx.path, "t", "a keyframe");
        FieldResult event = require_field(keyframe, ctx.path, "event", "a keyframe");
        if (time.error.has_value() || event.error.has_value()) {
            ctx.fail(time.error.has_value() ? std::move(*time.error) : std::move(*event.error));
            return;
        }
        Parsed<double> seconds = get_float(*time.node, ctx.path);
        Parsed<std::string> name = get_name(*event.node, ctx.path);
        if (seconds.error.has_value() || name.error.has_value()) {
            ctx.fail(seconds.error.has_value() ? std::move(*seconds.error)
                                               : std::move(*name.error));
            return;
        }
        // `key:` (m1-scene-format "symbolic keys"): validated against the
        // SAME self/root/global/group vocabulary a transition pair's `key:`
        // uses. self/root already match the runtime's existing "always the
        // host channel" trigger behavior exactly (core/statechart/
        // sequences.cpp); this is grammar-acceptance for the authored
        // spelling, not a new emit-routing feature (see validate_key's
        // comment on this file's scope boundary).
        if (const YamlNode* key = keyframe.find("key")) {
            validate_key(ctx, *key);
            if (ctx.failed())
                return;
        }
        statechart::TriggerTrackDesc trigger;
        trigger.time = seconds.value;
        trigger.event = base::Name(name.value);
        ctx.uses.push_back(EventUse{name.value, event.node->line, event.node->col});
        if (const YamlNode* payload = keyframe.find("payload")) {
            Parsed<base::Json> json = yaml_to_json(*payload, ctx.path);
            if (json.error.has_value()) {
                ctx.fail(std::move(*json.error));
                return;
            }
            if (!json.value.is_object()) {
                ctx.fail(
                    err_node("loader.bad_value", ctx.path, *payload, "payload must be a mapping"));
                return;
            }
            trigger.payload = std::move(json.value);
        }
        sheet.triggers.push_back(std::move(trigger));
    }
}

void parse_span_track(MachineCtx& ctx, const YamlNode& node, statechart::SequenceDesc& sheet) {
    if (!node.is_map()) {
        ctx.fail(
            err_node("loader.bad_value", ctx.path, node, "expected a {name|state, from, to} span"));
        return;
    }
    // `state:` is an accepted ALIAS for `name:` (examples/warden/brains/
    // warden.machine.yaml's spelling — the span always names the substate
    // it opens, D-BUILD-081's span->substate binding, so `state:` reads at
    // least as clearly; canonical re-emission still spells it `name:`,
    // machine_emit.cpp, exactly like `on:`/`Transition:`). Carrying both is
    // ambiguous, so it refuses.
    const YamlNode* name_field = node.find("name");
    const YamlNode* state_field = node.find("state");
    if (name_field != nullptr && state_field != nullptr) {
        ctx.fail(err_node("loader.bad_value",
                          ctx.path,
                          node,
                          "a span carries both 'name:' and 'state:' (state: is name:'s alias; "
                          "author exactly one)"));
        return;
    }
    static constexpr std::array<std::string_view, 3> kAllowedByName = {"name", "from", "to"};
    static constexpr std::array<std::string_view, 3> kAllowedByState = {"state", "from", "to"};
    if (auto error =
            check_keys(node, ctx.path, state_field != nullptr ? kAllowedByState : kAllowedByName)) {
        ctx.fail(std::move(*error));
        return;
    }
    FieldResult name =
        require_field(node, ctx.path, state_field != nullptr ? "state" : "name", "a span track");
    FieldResult from = require_field(node, ctx.path, "from", "a span track");
    FieldResult to = require_field(node, ctx.path, "to", "a span track");
    if (name.error.has_value() || from.error.has_value() || to.error.has_value()) {
        ctx.fail(name.error.has_value()
                     ? std::move(*name.error)
                     : (from.error.has_value() ? std::move(*from.error) : std::move(*to.error)));
        return;
    }
    Parsed<std::string> span_name = get_name(*name.node, ctx.path);
    Parsed<double> begin = get_float(*from.node, ctx.path);
    Parsed<double> end = get_float(*to.node, ctx.path);
    if (span_name.error.has_value() || begin.error.has_value() || end.error.has_value()) {
        ctx.fail(span_name.error.has_value()
                     ? std::move(*span_name.error)
                     : (begin.error.has_value() ? std::move(*begin.error) : std::move(*end.error)));
        return;
    }
    for (const statechart::SpanTrackDesc& existing : sheet.spans) {
        if (existing.name.view() == span_name.value) {
            ctx.fail(err_node("loader.duplicate",
                              ctx.path,
                              *name.node,
                              "span '" + span_name.value + "' is already declared"));
            return;
        }
    }
    statechart::SpanTrackDesc span;
    span.name = base::Name(span_name.value);
    span.begin = begin.value;
    span.end = end.value;
    ctx.derive(span_name.value + ".opened");
    ctx.derive(span_name.value + ".closed");
    sheet.spans.push_back(span);
}

} // namespace

std::vector<statechart::TransitionDesc> parse_pair_list(MachineCtx& ctx,
                                                        RegionCtx& region,
                                                        const YamlNode& node,
                                                        std::string_view owner_state) {
    std::vector<statechart::TransitionDesc> pairs;
    if (!node.is_seq()) {
        ctx.fail(err_node("loader.bad_value", ctx.path, node, "expected a list of pairs"));
        return pairs;
    }
    for (const YamlNode& element : node.seq) {
        pairs.push_back(parse_pair(ctx, region, element, owner_state));
        if (ctx.failed())
            break;
    }
    return pairs;
}

SequenceParse parse_sequence(MachineCtx& ctx, RegionCtx& region, const YamlNode& node) {
    SequenceParse out;
    if (!node.is_map()) {
        ctx.fail(err_node("loader.bad_value", ctx.path, node, "expected a sequence mapping"));
        return out;
    }
    static constexpr std::array<std::string_view, 5> kAllowed = {
        "duration", "end", "loop", "then", "tracks"};
    if (auto error = check_keys(node, ctx.path, kAllowed)) {
        ctx.fail(std::move(*error));
        return out;
    }
    FieldResult duration = require_field(node, ctx.path, "duration", "a sequence");
    if (duration.error.has_value()) {
        ctx.fail(std::move(*duration.error));
        return out;
    }
    Parsed<double> seconds = get_float(*duration.node, ctx.path);
    if (seconds.error.has_value()) {
        ctx.fail(std::move(*seconds.error));
        return out;
    }
    out.sheet.duration = seconds.value;

    if (const YamlNode* end = node.find("end")) {
        Parsed<std::string> mode = get_name(*end, ctx.path);
        if (mode.error.has_value()) {
            ctx.fail(std::move(*mode.error));
            return out;
        }
        if (mode.value == "finish")
            out.sheet.end = statechart::SequenceEnd::kFinish;
        else if (mode.value == "loop")
            out.sheet.end = statechart::SequenceEnd::kLoop;
        else if (mode.value == "hold")
            out.sheet.end = statechart::SequenceEnd::kHold;
        else {
            ctx.fail(err_node("loader.bad_value",
                              ctx.path,
                              *end,
                              "unknown end mode '" + mode.value + "' (finish, loop, hold)"));
            return out;
        }
    }
    if (const YamlNode* loop = node.find("loop")) {
        Parsed<std::int64_t> count = get_int(*loop, ctx.path);
        if (count.error.has_value()) {
            ctx.fail(std::move(*count.error));
            return out;
        }
        if (out.sheet.end != statechart::SequenceEnd::kLoop || count.value < 0 ||
            count.value > std::numeric_limits<std::uint32_t>::max()) {
            ctx.fail(err_node("loader.bad_value",
                              ctx.path,
                              *loop,
                              "loop: needs end: loop and a count >= 0 (0 = forever)"));
            return out;
        }
        out.sheet.loop_count = static_cast<std::uint32_t>(count.value);
    }
    if (const YamlNode* tracks = node.find("tracks")) {
        if (!tracks->is_seq()) {
            ctx.fail(err_node("loader.bad_value", ctx.path, *tracks, "expected a list of tracks"));
            return out;
        }
        for (const YamlNode& track : tracks->seq) {
            if (!track.is_map() || track.map.size() != 1) {
                ctx.fail(err_node("loader.bad_value",
                                  ctx.path,
                                  track,
                                  "expected one {trigger: ...} or {span: ...} entry"));
                return out;
            }
            const YamlEntry& entry = track.map.front();
            if (entry.key == "trigger")
                parse_trigger_track(ctx, entry.node(), out.sheet);
            else if (entry.key == "span")
                parse_span_track(ctx, entry.node(), out.sheet);
            else
                ctx.fail(err_at("loader.unknown_key",
                                ctx.path,
                                entry.key_line,
                                entry.key_col,
                                "unknown track kind '" + entry.key + "' (allowed: trigger, span)"));
            if (ctx.failed())
                return out;
        }
    }
    if (const YamlNode* then = node.find("then")) {
        Parsed<std::string> target = get_name(*then, ctx.path);
        if (target.error.has_value()) {
            ctx.fail(std::move(*target.error));
            return out;
        }
        const bool finishes =
            out.sheet.end == statechart::SequenceEnd::kFinish ||
            (out.sheet.end == statechart::SequenceEnd::kLoop && out.sheet.loop_count > 0);
        if (!finishes) {
            ctx.fail(err_node("loader.bad_value",
                              ctx.path,
                              *then,
                              "then: needs an end mode that finishes (finish, or loop with a "
                              "count)"));
            return out;
        }
        region.targets.push_back(TargetRef{target.value, then->line, then->col});
        out.then_target = std::move(target.value);
    }
    return out;
}

} // namespace midday::loader::detail
