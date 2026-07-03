#include "core/reflect/type_model.h"

#include "core/reflect/fatal.h"

#include <cstddef>
#include <string>

namespace midday::reflect {
namespace {

// Indexed by TypeKind — the single spelling table parse() and to_string()
// share, so they cannot drift apart.
constexpr std::string_view kKindNames[] = {
    "bool",
    "int",
    "float",
    "string",
    "name",
    "vec2",
    "vec3",
    "vec4",
    "quat",
    "color",
    "entity_ref",
    "asset_ref",
    "array",
    "map",
};
constexpr std::size_t kKindCount = std::size(kKindNames);

// Fixed-width numeric aggregate: [n] numbers, nothing else.
bool is_numeric_tuple(const base::Json& value, std::size_t n) {
    if (!value.is_array() || value.elements().size() != n)
        return false;
    for (const base::Json& element : value.elements())
        if (!element.is_number())
            return false;
    return true;
}

} // namespace

std::string_view to_string(TypeKind kind) {
    return kKindNames[static_cast<std::size_t>(kind)];
}

TypeDesc TypeDesc::scalar(TypeKind kind) {
    if (kind == TypeKind::kArray || kind == TypeKind::kMap)
        detail::fatal("TypeDesc::scalar called with composite kind '" +
                      std::string(to_string(kind)) + "' — use array_of/map_of");
    return {kind, {}};
}

TypeDesc TypeDesc::array_of(TypeDesc element) {
    return TypeDesc(TypeKind::kArray, {std::move(element)});
}

TypeDesc TypeDesc::map_of(TypeDesc value) {
    return TypeDesc(TypeKind::kMap, {std::move(value)});
}

const TypeDesc& TypeDesc::element() const {
    if (!is_composite())
        detail::fatal("TypeDesc::element on scalar '" + canonical() + "'");
    return element_[0];
}

std::string TypeDesc::canonical() const {
    std::string out(to_string(kind_));
    if (is_composite()) {
        out += '<';
        out += element_[0].canonical();
        out += '>';
    }
    return out;
}

std::optional<TypeDesc> TypeDesc::parse(std::string_view text) {
    const std::size_t open = text.find('<');
    const std::string_view head = open == std::string_view::npos ? text : text.substr(0, open);

    std::size_t kind_index = kKindCount;
    for (std::size_t i = 0; i < kKindCount; ++i) {
        if (kKindNames[i] == head) {
            kind_index = i;
            break;
        }
    }
    if (kind_index == kKindCount)
        return std::nullopt;
    const auto kind = static_cast<TypeKind>(kind_index);
    const bool composite = kind == TypeKind::kArray || kind == TypeKind::kMap;

    if (open == std::string_view::npos)
        return composite ? std::nullopt : std::optional<TypeDesc>(TypeDesc(kind, {}));

    // "<...>" present: only composites take it, and it must close at the end.
    if (!composite || text.size() < open + 2 || text.back() != '>')
        return std::nullopt;
    auto element = parse(text.substr(open + 1, text.size() - open - 2));
    if (!element)
        return std::nullopt;
    return kind == TypeKind::kArray ? array_of(std::move(*element)) : map_of(std::move(*element));
}

bool TypeDesc::accepts(const base::Json& value) const {
    switch (kind_) {
    case TypeKind::kBool:
        return value.is_bool();
    case TypeKind::kInt:
        return value.is_int();
    case TypeKind::kFloat:
        return value.is_number();
    case TypeKind::kString:
    case TypeKind::kName:
    case TypeKind::kEntityRef: // symbolic key: self/root/global/<group>
    case TypeKind::kAssetRef:  // project-root-relative path
        return value.is_string();
    case TypeKind::kVec2:
        return is_numeric_tuple(value, 2);
    case TypeKind::kVec3:
        return is_numeric_tuple(value, 3);
    case TypeKind::kVec4:
    case TypeKind::kQuat:
    case TypeKind::kColor:
        return is_numeric_tuple(value, 4);
    case TypeKind::kArray: {
        if (!value.is_array())
            return false;
        for (const base::Json& item : value.elements())
            if (!element_[0].accepts(item))
                return false;
        return true;
    }
    case TypeKind::kMap: {
        if (!value.is_object())
            return false;
        for (const auto& [key, item] : value.items())
            if (!element_[0].accepts(item))
                return false;
        return true;
    }
    }
    return false; // unreachable; kept for MSVC's control-path analysis
}

} // namespace midday::reflect
