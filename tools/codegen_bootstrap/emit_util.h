// tools/codegen_bootstrap/emit_util.h — internal document accessors shared
// by the four emitters. Every accessor presumes the shape load_document
// validated (api/CODEGEN.md "Validation order"); emitters never see garbage.

#pragma once

#include "core/base/json.h"

#include <string>
#include <string_view>

namespace midday::codegen::detail {

using base::Json;

inline const Json::Array& entries(const Json& parent, std::string_view key) {
    return parent.find(key)->elements();
}

inline const std::string& str(const Json& obj, std::string_view key) {
    return obj.find(key)->as_string();
}

// Optional doc/summary text: empty when absent (absent and empty emit alike).
inline std::string_view text(const Json& obj, std::string_view key) {
    const Json* value = obj.find(key);
    return value != nullptr && value->is_string() ? std::string_view(value->as_string())
                                                  : std::string_view();
}

inline bool truthy(const Json& obj, std::string_view key) {
    const Json* value = obj.find(key);
    return value != nullptr && value->is_bool() && value->as_bool();
}

} // namespace midday::codegen::detail
