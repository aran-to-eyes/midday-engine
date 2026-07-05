// core/loader/machine_ctx.h — INTERNAL shared state of the machine-file
// loader, split across machine_load.cpp (file / region / state structure)
// and machine_parts.cpp (pairs + sequences) to hold the 500-line ratchet.
// Not installed API; loader.h is the public surface.

#pragma once

#include "core/base/error.h"
#include "core/expr/env.h"
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
    const EventsDecl& vocab;
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

} // namespace midday::loader::detail
