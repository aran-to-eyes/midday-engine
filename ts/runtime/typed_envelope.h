// ts/runtime/typed_envelope.h — the typed value envelope the component host
// hands across the JSON hook seam (M2 0B, #12b). JSON cannot carry an
// EntityRef INSTANCE or a Vec3 in its script-facing {x, y, z} shape, and a
// blind JS-side walk could never distinguish a payload's entity_ref bits
// from an ordinary integer — so the C++ side, which HOLDS the schema
// (reflect::EventDesc for payloads, a manifest TypeDesc for authored
// fields), encodes every value as a small tagged node and the component prelude
// decodes purely structurally:
//   {t:"json", v:<verbatim>}        bool / int / float / string / name
//   {t:"ref",  i:<index>, g:<gen>}  entity_ref  -> new EntityRef(i, g)
//   {t:"vec2"|"vec3", v:[..]}       -> {x, y[, z]}
//   {t:"vec4"|"quat", v:[..]}       -> {x, y, z, w}
//   {t:"color", v:[..]}             -> {r, g, b, a}
//   {t:"arr", v:[nodes]}            element-typed recursion, order kept
//   {t:"map", v:{key: node}}        value-typed recursion
// Encoding happens ONLY at schema-typed positions — tags can never collide
// with user data, whatever keys it uses.

#pragma once

#include "core/base/error.h"
#include "core/base/json.h"
#include "core/reflect/registry.h"
#include "core/reflect/type_model.h"

#include <optional>
#include <string_view>

namespace midday::script {

struct EncodedValue {
    base::Json node;                  // meaningless when error engaged
    std::optional<base::Error> error; // script.hydrate — value ∉ the type's wire shape
};

// One value in its RUNTIME wire shape (entity_ref = to_bits integer, vec3 =
// [x, y, z] — the bus's runtime_accepts model) -> its envelope node.
// `field_path` names the position in diagnostics ("self", "hits[2]").
EncodedValue encode_typed_value(const reflect::TypeDesc& type,
                                const base::Json& value,
                                std::string_view field_path);

// A whole event payload against its schema, SCHEMA DECLARATION ORDER (never
// JSON insertion order). A null schema (unregistered event name — custom
// key-scoped vocabularies are legal, D-BUILD-046) passes the payload
// through verbatim as one {t:"json"} node: no hydration without a schema.
EncodedValue encode_event_payload(const reflect::EventDesc* schema, const base::Json& payload);

} // namespace midday::script
