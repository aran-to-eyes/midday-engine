// core/reflect/type_model.h — the data-driven property type model: one
// vocabulary of types shared by reflection, the text formats, and codegen
// (spec section 8: schemas are generated FROM reflection, never hand-kept).
//
// Contract:
//   * A TypeDesc is a value: scalar kinds carry nothing, array<T>/map<T>
//     carry exactly one element type. New kinds extend the enum without
//     reshaping the model — downstream nodes only ADD.
//   * One canonical spelling everywhere ("float", "entity_ref",
//     "array<vec3>", "map<string>"): canonical() emits it, parse() reads it
//     back (strict; round-trip is pinned by tests). engine_api.json and the
//     schema manifest use this spelling verbatim.
//   * accepts() answers whether a JSON literal (property default, payload
//     field value) inhabits the type — the seed of validate-before-write.
//     Map keys are strings by contract (JSON objects).

#pragma once

#include "core/base/json.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace midday::reflect {

enum class TypeKind : std::uint8_t {
    kBool,
    kInt,    // int64 semantics (core JSON integer model, D-BUILD-012)
    kFloat,  // real_t semantics; JSON carries it as a number
    kString, // UTF-8
    kName,   // interned Name; serialized as its string content
    kVec2,
    kVec3,
    kVec4,
    kQuat,  // [x, y, z, w]
    kColor, // [r, g, b, a]
    kEntityRef,
    kAssetRef,
    kArray, // composite: array<element>
    kMap,   // composite: map<value>, string keys
};

// Canonical spelling of a kind ("entity_ref", "array", ...).
std::string_view to_string(TypeKind kind);

class TypeDesc {
public:
    // Factories enforce the invariant (scalar kinds have no element,
    // composites have exactly one); malformed construction aborts loudly —
    // registry paths never throw.
    static TypeDesc scalar(TypeKind kind); // pre: kind is not kArray/kMap
    static TypeDesc array_of(TypeDesc element);
    static TypeDesc map_of(TypeDesc value);

    [[nodiscard]] TypeKind kind() const { return kind_; }

    [[nodiscard]] bool is_composite() const {
        return kind_ == TypeKind::kArray || kind_ == TypeKind::kMap;
    }

    // The element type (array) / value type (map). Pre: is_composite().
    [[nodiscard]] const TypeDesc& element() const;

    // The one canonical spelling: "float", "array<vec3>", "map<array<int>>".
    [[nodiscard]] std::string canonical() const;

    // Strict inverse of canonical(); anything else -> nullopt (no partial
    // consumption, no whitespace tolerance — agent formats are exact).
    static std::optional<TypeDesc> parse(std::string_view text);

    // Does `value` (a JSON literal) inhabit this type? Defaults use this at
    // registration; the null JSON value never inhabits (null means "absent").
    [[nodiscard]] bool accepts(const base::Json& value) const;

    friend bool operator==(const TypeDesc& a, const TypeDesc& b) {
        return a.kind_ == b.kind_ && a.element_ == b.element_;
    }

private:
    TypeDesc(TypeKind kind, std::vector<TypeDesc> element)
        : kind_(kind), element_(std::move(element)) {}

    TypeKind kind_;
    std::vector<TypeDesc> element_; // empty for scalars, exactly one for composites
};

} // namespace midday::reflect
