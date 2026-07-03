#include "core/expr/value.h"

namespace midday::expr {

std::string_view to_string(ValueType type) {
    switch (type) {
    case ValueType::kBool:
        return "bool";
    case ValueType::kInt:
        return "int";
    case ValueType::kFloat:
        return "float";
    case ValueType::kString:
        return "string";
    case ValueType::kName:
        return "name";
    case ValueType::kVec2:
        return "vec2";
    case ValueType::kVec3:
        return "vec3";
    case ValueType::kVec4:
        return "vec4";
    case ValueType::kQuat:
        return "quat";
    }
    return "?";
}

reflect::TypeDesc to_type_desc(ValueType type) {
    switch (type) {
    case ValueType::kBool:
        return reflect::TypeDesc::scalar(reflect::TypeKind::kBool);
    case ValueType::kInt:
        return reflect::TypeDesc::scalar(reflect::TypeKind::kInt);
    case ValueType::kFloat:
        return reflect::TypeDesc::scalar(reflect::TypeKind::kFloat);
    case ValueType::kString:
        return reflect::TypeDesc::scalar(reflect::TypeKind::kString);
    case ValueType::kName:
        return reflect::TypeDesc::scalar(reflect::TypeKind::kName);
    case ValueType::kVec2:
        return reflect::TypeDesc::scalar(reflect::TypeKind::kVec2);
    case ValueType::kVec3:
        return reflect::TypeDesc::scalar(reflect::TypeKind::kVec3);
    case ValueType::kVec4:
        return reflect::TypeDesc::scalar(reflect::TypeKind::kVec4);
    case ValueType::kQuat:
        return reflect::TypeDesc::scalar(reflect::TypeKind::kQuat);
    }
    return reflect::TypeDesc::scalar(reflect::TypeKind::kBool); // unreachable
}

int lane_count(ValueType type) {
    switch (type) {
    case ValueType::kVec2:
        return 2;
    case ValueType::kVec3:
        return 3;
    case ValueType::kVec4:
    case ValueType::kQuat:
        return 4;
    default:
        return 0;
    }
}

} // namespace midday::expr
