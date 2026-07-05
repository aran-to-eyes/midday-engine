// core/loader/machine_emit.cpp — machine_emit.h: MachineFile -> canonical
// YamlNode tree.
//
// Quoting policy (yaml_build.h's "quoting is the caller's job"): numbers and
// booleans emit UNQUOTED (a quoted "1.2"/"true" reparses as a STRING, never
// a number/bool — yaml.h's "quoted scalars are always strings" rule); every
// other scalar (names, event names, `if:` expressions, component field
// strings) emits QUOTED unconditionally — get_name/get_string accept either
// quoting, so this is always safe and never loses information.

#include "core/loader/machine_emit.h"

#include "core/expr/value.h"
#include "core/loader/yaml_build.h"
#include "core/statechart/machine_desc.h"

#include <array>
#include <charconv>
#include <string>
#include <utility>
#include <vector>

namespace midday::loader {
namespace {

YamlNode text_scalar(std::string_view text) {
    return make_scalar(std::string(text), /*quoted=*/true);
}

YamlNode number_scalar(double value) {
    return make_scalar(base::Json(value).dump(), /*quoted=*/false);
}

YamlNode int_scalar(std::int64_t value) {
    return make_scalar(base::Json(value).dump(), /*quoted=*/false);
}

YamlNode bool_scalar(bool value) {
    return make_scalar(value ? "true" : "false", /*quoted=*/false);
}

YamlNode json_to_yaml(const base::Json& value) {
    if (value.is_bool())
        return bool_scalar(value.as_bool());
    if (value.is_int())
        return int_scalar(value.as_int());
    if (value.is_double())
        return number_scalar(value.as_double());
    if (value.is_string())
        return text_scalar(value.as_string());
    if (value.is_array()) {
        std::vector<YamlNode> items;
        for (const base::Json& element : value.elements())
            items.push_back(json_to_yaml(element));
        return make_seq(std::move(items));
    }
    if (value.is_object()) {
        std::vector<YamlEntry> entries;
        for (const auto& [key, entry_value] : value.items())
            entries.push_back(make_entry(key, json_to_yaml(entry_value)));
        return make_map(std::move(entries));
    }
    return YamlNode{}; // null
}

// Vec3 fields are `float` (real_t), never `double` (core/math/vec.h) — going
// through base::Json(double) would WIDEN each component first, and the
// shortest-round-trip-for-double algorithm then has to spell out the
// widened value exactly (`1.2f` widens to `1.2000000476837158`, a real
// double with no shorter double spelling — not a bug, just the wrong
// precision to be shortest FOR). std::to_chars<float> computes the
// shortest decimal that round-trips to the SAME float32 value instead.
YamlNode float_scalar(float value) {
    std::array<char, 32> buffer{};
    const std::to_chars_result result =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    return make_scalar(std::string(buffer.data(), result.ptr), /*quoted=*/false);
}

YamlNode vec3_to_yaml(const math::Vec3& value) {
    return make_seq({float_scalar(value.x), float_scalar(value.y), float_scalar(value.z)});
}

YamlNode pair_to_yaml(const statechart::TransitionDesc& pair) {
    std::vector<YamlEntry> entries;
    entries.push_back(make_entry("event", text_scalar(pair.event.view())));
    entries.push_back(make_entry("goto", text_scalar(pair.target.view())));
    entries.push_back(make_entry("priority", int_scalar(pair.priority)));
    if (!pair.condition.empty())
        entries.push_back(make_entry("if", text_scalar(pair.condition)));
    return make_map(std::move(entries));
}

YamlNode transitions_to_yaml(const std::vector<statechart::TransitionDesc>& pairs) {
    std::vector<YamlNode> items;
    items.reserve(pairs.size());
    for (const statechart::TransitionDesc& pair : pairs)
        items.push_back(pair_to_yaml(pair));
    return make_seq(std::move(items));
}

template <typename ComponentEntry>
YamlNode components_to_yaml(const std::vector<ComponentEntry>& components) {
    std::vector<YamlNode> items;
    items.reserve(components.size());
    for (const ComponentEntry& component : components)
        items.push_back(make_map(
            {make_entry(std::string(component.type.view()), json_to_yaml(component.fields))}));
    return make_seq(std::move(items));
}

std::string_view sequence_end_name(statechart::SequenceEnd end) {
    switch (end) {
    case statechart::SequenceEnd::kFinish:
        return "finish";
    case statechart::SequenceEnd::kLoop:
        return "loop";
    case statechart::SequenceEnd::kHold:
        return "hold";
    }
    return "finish";
}

YamlNode sequence_to_yaml(const statechart::SequenceDesc& sequence) {
    std::vector<YamlEntry> entries;
    entries.push_back(make_entry("duration", number_scalar(sequence.duration)));
    entries.push_back(make_entry("end", text_scalar(sequence_end_name(sequence.end))));
    if (sequence.end == statechart::SequenceEnd::kLoop)
        entries.push_back(
            make_entry("loop", int_scalar(static_cast<std::int64_t>(sequence.loop_count))));
    if (!sequence.triggers.empty() || !sequence.spans.empty()) {
        std::vector<YamlNode> tracks;
        for (const statechart::TriggerTrackDesc& trigger : sequence.triggers) {
            std::vector<YamlEntry> keyframe = {
                make_entry("t", number_scalar(trigger.time)),
                make_entry("event", text_scalar(trigger.event.view())),
            };
            if (!trigger.payload.is_null() &&
                !(trigger.payload.is_object() && trigger.payload.items().empty()))
                keyframe.push_back(make_entry("payload", json_to_yaml(trigger.payload)));
            tracks.push_back(make_map({make_entry("trigger", make_seq({make_map(keyframe)}))}));
        }
        for (const statechart::SpanTrackDesc& span : sequence.spans) {
            std::vector<YamlEntry> span_entries = {
                make_entry("name", text_scalar(span.name.view())),
                make_entry("from", number_scalar(span.begin)),
                make_entry("to", number_scalar(span.end)),
            };
            tracks.push_back(make_map({make_entry("span", make_map(span_entries))}));
        }
        entries.push_back(make_entry("tracks", make_seq(std::move(tracks))));
    }
    return make_map(std::move(entries));
}

YamlNode child_to_yaml(const StateChildDesc& child) {
    std::vector<YamlEntry> entries;
    entries.push_back(make_entry("entity", text_scalar(child.entity.view())));
    entries.push_back(make_entry("at", vec3_to_yaml(child.at.translation)));
    if (!child.components.empty())
        entries.push_back(make_entry("components", components_to_yaml(child.components)));
    return make_map(std::move(entries));
}

const StateChildren* find_children(const std::vector<StateChildren>& all,
                                   std::string_view region,
                                   std::string_view state) {
    for (const StateChildren& entry : all)
        if (entry.region.view() == region && entry.state.view() == state)
            return &entry;
    return nullptr;
}

const StateScriptRef* find_script(const std::vector<StateScriptRef>& all,
                                  std::string_view region,
                                  std::string_view state) {
    for (const StateScriptRef& entry : all)
        if (entry.region.view() == region && entry.state.view() == state)
            return &entry;
    return nullptr;
}

YamlNode state_to_yaml(const statechart::StateDesc& state,
                       std::string_view region,
                       const MachineFile& machine) {
    std::vector<YamlEntry> entries;
    if (const StateScriptRef* script = find_script(machine.scripts, region, state.name.view()))
        entries.push_back(make_entry("script", text_scalar(script->ref)));
    entries.push_back(make_entry("history", bool_scalar(state.history)));
    if (!state.transitions.empty())
        entries.push_back(make_entry("Transition", transitions_to_yaml(state.transitions)));
    if (!state.components.empty())
        entries.push_back(make_entry("components", components_to_yaml(state.components)));
    if (state.sequence.has_value())
        entries.push_back(make_entry("sequence", sequence_to_yaml(*state.sequence)));
    if (const StateChildren* children =
            find_children(machine.children, region, state.name.view())) {
        std::vector<YamlNode> items;
        items.reserve(children->children.size());
        for (const StateChildDesc& child : children->children)
            items.push_back(child_to_yaml(child));
        entries.push_back(make_entry("children", make_seq(std::move(items))));
    }
    if (!state.substates.empty()) {
        entries.push_back(make_entry("initial", text_scalar(state.initial.view())));
        std::vector<YamlEntry> substates;
        substates.reserve(state.substates.size());
        for (const statechart::StateDesc& substate : state.substates)
            substates.push_back(make_entry(std::string(substate.name.view()),
                                           state_to_yaml(substate, region, machine)));
        entries.push_back(make_entry("states", make_map(std::move(substates))));
    }
    return make_map(std::move(entries));
}

YamlNode region_to_yaml(const statechart::RegionDesc& region, const MachineFile& machine) {
    std::vector<YamlEntry> entries;
    entries.push_back(make_entry("initial", text_scalar(region.initial.view())));
    entries.push_back(make_entry("history", bool_scalar(region.history)));
    if (!region.any_state.empty())
        entries.push_back(make_entry("anystate", transitions_to_yaml(region.any_state)));
    std::vector<YamlEntry> states;
    states.reserve(region.states.size());
    for (const statechart::StateDesc& state : region.states)
        states.push_back(make_entry(std::string(state.name.view()),
                                    state_to_yaml(state, region.name.view(), machine)));
    entries.push_back(make_entry("states", make_map(std::move(states))));
    return make_map(std::move(entries));
}

std::string_view value_type_name(expr::ValueType type) {
    return expr::to_string(type);
}

} // namespace

YamlNode machine_to_yaml(const MachineFile& machine) {
    std::vector<YamlEntry> root = {
        make_entry("format", int_scalar(1)),
        make_entry("machine", text_scalar(machine.desc.name.view())),
    };
    if (!machine.desc.vars.empty()) {
        std::vector<YamlEntry> vars;
        vars.reserve(machine.desc.vars.size());
        for (const statechart::VarDesc& var : machine.desc.vars)
            vars.push_back(make_entry(var.name, text_scalar(value_type_name(var.type))));
        root.push_back(make_entry("vars", make_map(std::move(vars))));
    }
    std::vector<YamlEntry> regions;
    regions.reserve(machine.desc.regions.size());
    for (const statechart::RegionDesc& region : machine.desc.regions)
        regions.push_back(
            make_entry(std::string(region.name.view()), region_to_yaml(region, machine)));
    root.push_back(make_entry("regions", make_map(std::move(regions))));
    return make_map(std::move(root));
}

} // namespace midday::loader
