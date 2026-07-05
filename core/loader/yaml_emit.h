// core/loader/yaml_emit.h — the canonical strict-YAML emitter (m1-strict-
// yaml): the exact counterpart of yaml.h's parser. Serializes a YamlNode
// tree back to text with ONE deterministic rendering, independent of how the
// source was authored (flow vs block, quote style) and independent of any
// format schema — `midday fmt` runs this on any strict-YAML file, scene or
// machine or a format nobody has invented yet.
//
// Canonical rules (the whole contract; a reviewer can check every rule here):
//   * Every mapping and sequence renders BLOCK style, one entry per line,
//     2-space indent per nesting level — flow style (`{...}`/`[...]`) is an
//     authoring convenience the parser accepts but the canonical form never
//     reintroduces, EXCEPT that an EMPTY mapping/sequence renders `{}`/`[]`
//     (block style cannot express empty collections).
//   * Key order is authoring order (yaml.h's contract already holds it) —
//     the emitter never reorders keys.
//   * A scalar's `quoted` flag is preserved exactly: quoted values always
//     re-emit double-quoted (JSON-style escapes); unquoted values re-emit
//     VERBATIM (the parser already proved they're safe plain scalars in
//     their original context, and block style is only ever LESS
//     restrictive than the flow style they might have been written in) —
//     except a raw embedded newline, which forces double-quoted emission
//     (a plain scalar cannot carry one).
//   * Mapping keys are plain unless the text would be unsafe as a plain
//     scalar (empty, leading/trailing whitespace, a YAML indicator as the
//     first character, or an embedded ": "), in which case they are
//     double-quoted the same way.
//   * A null value emits nothing after `key:` (or after the sequence dash),
//     matching the "key:" / "- " authoring forms yaml.h already treats as
//     null.
// Idempotence follows by construction: emit(parse(emit(x))) == emit(x),
// because every rule above is a pure function of a node's own kind/quoted/
// text — never of how it happened to be formatted before.

#pragma once

#include "core/loader/yaml.h"

#include <string>

namespace midday::loader {

[[nodiscard]] std::string emit_yaml(const YamlNode& root);

} // namespace midday::loader
