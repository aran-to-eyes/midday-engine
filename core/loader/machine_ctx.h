// core/loader/machine_ctx.h — INTERNAL shared state of the machine-file
// loader, split across machine_load.cpp (file / region / state structure)
// and machine_parts.cpp (pairs + sequences) to hold the 500-line ratchet.
// Not installed API; loader.h is the public surface.

#pragma once

#include "core/base/error.h"
#include "core/expr/env.h"
#include "core/loader/component_vocab.h"
#include "core/loader/gaps.h"
#include "core/loader/loader.h"
#include "core/loader/yaml.h"
#include "core/statechart/machine_desc.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace midday::loader::detail {

// One pair/trigger event reference, checked against the vocabulary closure
// (declared ∪ built-in ∪ derived) once the whole machine is parsed.
struct EventUse {
    std::string event = {};
    int line = 0;
    int col = 0;
};

// One goto/then target, resolved region-wide once the region is parsed.
struct TargetRef {
    std::string target = {};
    int line = 0;
    int col = 0;
};

struct MachineCtx {
    const std::string& path;
    const std::string& root_dir;
    const reflect::Registry& registry;
    const EventsDecl& vocab;
    const ComponentVocab& components_vocab; // m1-scene-format: state components:
    bool lenient = false;                   // m1-scene-format: report, don't refuse
    MachineFile out = {};
    expr::EnvSpec env = {}; // the declared vars — `if:` filters compile here
    std::vector<EventUse> uses = {};
    std::vector<std::string> derived = {}; // <state>.finished, <span>.opened/.closed
    std::optional<base::Error> error = {}; // first refusal wins

    void fail(base::Error error_value) {
        if (!error.has_value())
            error = std::move(error_value);
    }

    [[nodiscard]] bool failed() const { return error.has_value(); }

    void derive(std::string name) { derived.push_back(std::move(name)); }

    void add_channel(std::string_view name);

    // A gap in lenient mode; a hard refusal (`hard_error`) otherwise — the
    // ONE place machine_load.cpp/machine_parts.cpp decide which of the two
    // a "content the engine doesn't implement yet" finding becomes.
    void gap_or_fail(std::string kind,
                     std::string what,
                     int line,
                     int col,
                     std::string detail,
                     base::Error hard_error) {
        if (!lenient) {
            fail(std::move(hard_error));
            return;
        }
        out.gaps.push_back(Gap{.kind = std::move(kind),
                               .what = std::move(what),
                               .file = path,
                               .line = line,
                               .col = col,
                               .detail = std::move(detail)});
    }
};

struct RegionCtx {
    base::Name region;
    std::vector<std::string> state_names; // region-wide (all nesting levels)
    std::vector<TargetRef> targets;
};

// machine_parts.cpp — pair lists (`on:` / `anystate:`; owner_state empty for
// any-state rules) and dope sheets.
std::vector<statechart::TransitionDesc> parse_pair_list(MachineCtx& ctx,
                                                        RegionCtx& region,
                                                        const YamlNode& node,
                                                        std::string_view owner_state);

struct SequenceParse {
    statechart::SequenceDesc sheet;
    std::optional<std::string> then_target; // `then:` sugar, canonicalized by the caller
};

SequenceParse parse_sequence(MachineCtx& ctx, RegionCtx& region, const YamlNode& node);

// machine_components.cpp — the generic `components:` list a state or a
// state child may carry (m1-scene-format, spec 4.1 "states owning
// component sets"), and `children:` parsing (components-aware).
std::vector<statechart::StateComponentDesc> parse_state_components(MachineCtx& ctx,
                                                                   const YamlNode& node);
std::vector<GenericComponentEntry> parse_child_components(MachineCtx& ctx, const YamlNode& node);
void parse_children(MachineCtx& ctx, RegionCtx& region, base::Name state, const YamlNode& node);

} // namespace midday::loader::detail
