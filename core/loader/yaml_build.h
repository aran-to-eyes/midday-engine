// core/loader/yaml_build.h — tiny YamlNode tree builders for code that
// CONSTRUCTS a strict-YAML document directly rather than parsing one.
// Hoisted to a shared header on the file_io.h second-consumer rule: `midday
// check --fix`'s .uid sidecar writer (core/loader/uid.cpp) and
// `midday new`'s project scaffold (cli/verbs/new.cpp) both build small
// YamlNode trees by hand and feed them through yaml_emit.h so every
// generated file is born canonical.
//
// QUOTING IS THE CALLER'S JOB: yaml_emit.h trusts a scalar's `quoted` flag
// verbatim (it never re-derives safety, because for a PARSED node the
// parser already proved the original text was a safe plain scalar in
// context). A hand-built node carries no such proof — pass `quoted = true`
// for any value whose content is not a fixed, hand-verified-safe literal
// (free-form user text, anything that might start with a YAML indicator
// character or contain ": ").

#pragma once

#include "core/loader/yaml.h"

#include <string>
#include <utility>
#include <vector>

namespace midday::loader {

inline YamlNode make_scalar(std::string text, bool quoted = false) {
    YamlNode node;
    node.kind = YamlNode::Kind::kScalar;
    node.scalar = std::move(text);
    node.quoted = quoted;
    return node;
}

inline YamlEntry make_entry(std::string key, YamlNode value) {
    YamlEntry entry;
    entry.key = std::move(key);
    entry.value.push_back(std::move(value));
    return entry;
}

inline YamlNode make_map(std::vector<YamlEntry> entries) {
    YamlNode node;
    node.kind = YamlNode::Kind::kMap;
    node.map = std::move(entries);
    return node;
}

inline YamlNode make_seq(std::vector<YamlNode> elements) {
    YamlNode node;
    node.kind = YamlNode::Kind::kSeq;
    node.seq = std::move(elements);
    return node;
}

} // namespace midday::loader
