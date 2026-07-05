// core/loader/override.cpp — override.h: override-block parsing and the
// name-only resolution/application engine.

#include "core/loader/override.h"

#include "core/loader/parse_util.h"

#include <array>
#include <utility>

namespace midday::loader {

using detail::err_at;
using detail::err_node;
using detail::Parsed;

namespace {

std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> segments;
    std::string current;
    for (const char c : path) {
        if (c == '/') {
            segments.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    segments.push_back(current);
    return segments;
}

statechart::RegionDesc* find_region(std::vector<statechart::RegionDesc>& regions,
                                    std::string_view name) {
    for (statechart::RegionDesc& region : regions)
        if (region.name.view() == name)
            return &region;
    return nullptr;
}

// Direct children ONLY (region's top-level states, or one state's own
// substates) — never a flat "anywhere in the region" scan: a path can only
// resolve through the SAME parent/child structure the machine actually
// authored (header contract).
statechart::StateDesc* find_direct_state(std::vector<statechart::StateDesc>& states,
                                         std::string_view name) {
    for (statechart::StateDesc& state : states)
        if (state.name.view() == name)
            return &state;
    return nullptr;
}

StateChildren*
find_children(std::vector<StateChildren>& all, std::string_view region, std::string_view state) {
    for (StateChildren& entry : all)
        if (entry.region.view() == region && entry.state.view() == state)
            return &entry;
    return nullptr;
}

StateChildDesc* find_child(std::vector<StateChildDesc>& children, std::string_view name) {
    for (StateChildDesc& child : children)
        if (child.entity.view() == name)
            return &child;
    return nullptr;
}

GenericComponentEntry* find_component(std::vector<GenericComponentEntry>& components,
                                      std::string_view name) {
    for (GenericComponentEntry& component : components)
        if (component.type.view() == name)
            return &component;
    return nullptr;
}

statechart::StateComponentDesc*
find_state_component(std::vector<statechart::StateComponentDesc>& components,
                     std::string_view name) {
    for (statechart::StateComponentDesc& component : components)
        if (component.type.view() == name)
            return &component;
    return nullptr;
}

void merge_fields(base::Json& fields, const base::Json& diff) {
    for (const auto& [key, value] : diff.items())
        fields.set(key, value);
}

std::optional<base::Error> apply_sequence_diff(statechart::SequenceDesc& seq,
                                               const OverrideEntry& entry,
                                               std::string_view file) {
    for (const auto& [key, value] : entry.diff.items()) {
        if (key == "duration") {
            if (!value.is_number())
                return err_at("loader.bad_value",
                              file,
                              entry.line,
                              entry.col,
                              "override '" + entry.path + "': 'duration' must be a number");
            seq.duration = value.as_double();
        } else if (key == "end") {
            if (!value.is_string())
                return err_at("loader.bad_value",
                              file,
                              entry.line,
                              entry.col,
                              "override '" + entry.path + "': 'end' must be a string");
            const std::string& mode = value.as_string();
            if (mode == "finish")
                seq.end = statechart::SequenceEnd::kFinish;
            else if (mode == "loop")
                seq.end = statechart::SequenceEnd::kLoop;
            else if (mode == "hold")
                seq.end = statechart::SequenceEnd::kHold;
            else
                return err_at("loader.bad_value",
                              file,
                              entry.line,
                              entry.col,
                              "override '" + entry.path + "': unknown end mode '" + mode + "'");
        } else if (key == "loop") {
            if (!value.is_int() || value.as_int() < 0)
                return err_at("loader.bad_value",
                              file,
                              entry.line,
                              entry.col,
                              "override '" + entry.path + "': 'loop' must be a non-negative int");
            seq.loop_count = static_cast<std::uint32_t>(value.as_int());
        } else {
            return err_at("loader.unknown_key",
                          file,
                          entry.line,
                          entry.col,
                          "override '" + entry.path + "': 'sequence' has no diffable field '" +
                              key + "' (duration, end, loop)");
        }
    }
    return std::nullopt;
}

// One override entry, name-resolved and applied onto `machine`. Never
// downgraded to a Gap: an override path names something by hand, so a
// wrong name is an authoring bug (header contract).
std::optional<base::Error>
apply_one(MachineFile& machine, const OverrideEntry& entry, std::string_view file) {
    const std::vector<std::string> segments = split_path(entry.path);
    if (segments.size() < 2 || segments.front().empty())
        return err_at("loader.bad_value",
                      file,
                      entry.line,
                      entry.col,
                      "override path '" + entry.path + "' needs at least <region>/<leaf>");

    statechart::RegionDesc* region = find_region(machine.desc.regions, segments[0]);
    if (region == nullptr)
        return err_at("loader.bad_ref",
                      file,
                      entry.line,
                      entry.col,
                      "override '" + entry.path + "': unknown region '" + segments[0] + "'");

    std::vector<statechart::StateDesc>* container = &region->states;
    statechart::StateDesc* state = nullptr;
    std::size_t idx = 1;
    while (idx < segments.size()) {
        statechart::StateDesc* match = find_direct_state(*container, segments[idx]);
        if (match == nullptr)
            break;
        state = match;
        container = &match->substates;
        idx += 1;
    }
    if (state == nullptr)
        return err_at("loader.bad_ref",
                      file,
                      entry.line,
                      entry.col,
                      "override '" + entry.path + "': region '" + segments[0] +
                          "' has no top-level state '" + segments[1] + "'");

    const std::vector<std::string> leaf(segments.begin() + static_cast<std::ptrdiff_t>(idx),
                                        segments.end());
    if (leaf.size() == 1 && leaf[0] == "sequence") {
        if (!state->sequence.has_value())
            return err_at("loader.bad_ref",
                          file,
                          entry.line,
                          entry.col,
                          "override '" + entry.path + "': state '" +
                              std::string(state->name.view()) + "' has no sequence");
        return apply_sequence_diff(*state->sequence, entry, file);
    }
    if (leaf.size() == 1) {
        statechart::StateComponentDesc* component =
            find_state_component(state->components, leaf[0]);
        if (component == nullptr)
            return err_at("loader.bad_ref",
                          file,
                          entry.line,
                          entry.col,
                          "override '" + entry.path + "': component '" + leaf[0] +
                              "' is not declared on state '" + std::string(state->name.view()) +
                              "'");
        merge_fields(component->fields, entry.diff);
        return std::nullopt;
    }
    if (leaf.size() == 2) {
        StateChildren* children =
            find_children(machine.children, region->name.view(), state->name.view());
        if (children == nullptr)
            return err_at("loader.bad_ref",
                          file,
                          entry.line,
                          entry.col,
                          "override '" + entry.path + "': state '" +
                              std::string(state->name.view()) + "' has no children");
        StateChildDesc* child = find_child(children->children, leaf[0]);
        if (child == nullptr)
            return err_at("loader.bad_ref",
                          file,
                          entry.line,
                          entry.col,
                          "override '" + entry.path + "': no child entity '" + leaf[0] +
                              "' under state '" + std::string(state->name.view()) + "'");
        GenericComponentEntry* component = find_component(child->components, leaf[1]);
        if (component == nullptr)
            return err_at("loader.bad_ref",
                          file,
                          entry.line,
                          entry.col,
                          "override '" + entry.path + "': component '" + leaf[1] +
                              "' is not declared on child '" + leaf[0] + "'");
        merge_fields(component->fields, entry.diff);
        return std::nullopt;
    }
    return err_at("loader.bad_value",
                  file,
                  entry.line,
                  entry.col,
                  "override '" + entry.path + "': too many segments after state '" +
                      std::string(state->name.view()) + "'");
}

} // namespace

OverrideParseResult parse_override_block(const YamlNode& node, const std::string& file) {
    OverrideParseResult out;
    if (!node.is_map()) {
        out.error = err_node("loader.bad_value", file, node, "expected an {<path>: {...}} mapping");
        return out;
    }
    for (const YamlEntry& entry : node.map) {
        if (entry.key.empty()) {
            out.error = err_at("loader.bad_value",
                               file,
                               entry.key_line,
                               entry.key_col,
                               "an override path must not be empty");
            return out;
        }
        const YamlNode& body = entry.node();
        if (!body.is_map()) {
            out.error = err_node("loader.bad_value",
                                 file,
                                 body,
                                 "override '" + entry.key + "' must carry a property-diff mapping");
            return out;
        }
        Parsed<base::Json> diff = detail::yaml_to_json(body, file);
        if (diff.error.has_value()) {
            out.error = std::move(diff.error);
            return out;
        }
        out.entries.push_back(OverrideEntry{.path = entry.key,
                                            .diff = std::move(diff.value),
                                            .line = entry.key_line,
                                            .col = entry.key_col});
    }
    return out;
}

SplitOverrides split_overrides_for_machine(const std::vector<OverrideEntry>& overrides,
                                           std::string_view machine_name) {
    SplitOverrides out;
    for (const OverrideEntry& entry : overrides) {
        const std::size_t slash = entry.path.find('/');
        const std::string first =
            slash == std::string::npos ? entry.path : entry.path.substr(0, slash);
        if (first == machine_name && slash != std::string::npos) {
            OverrideEntry stripped = entry;
            stripped.path = entry.path.substr(slash + 1);
            out.matched.push_back(std::move(stripped));
        } else {
            out.unmatched.push_back(entry);
        }
    }
    return out;
}

ApplyOverridesResult apply_overrides(MachineFile base,
                                     const std::vector<OverrideEntry>& overrides,
                                     std::string_view origin_file) {
    ApplyOverridesResult out;
    out.machine = std::move(base);
    for (const OverrideEntry& entry : overrides) {
        if (auto error = apply_one(out.machine, entry, origin_file)) {
            out.error = std::move(error);
            return out;
        }
    }
    return out;
}

} // namespace midday::loader
