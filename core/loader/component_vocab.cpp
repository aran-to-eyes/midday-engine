// core/loader/component_vocab.cpp — component_vocab.h.

#include "core/loader/component_vocab.h"

#include "core/base/file_io.h"
#include "core/base/json.h"

namespace midday::loader {

bool is_native_component(std::string_view name) {
    return name == "Transform" || name == "Collider" || name == "RigidBody";
}

ComponentVocabLoadResult load_component_vocab(const std::string& path) {
    ComponentVocabLoadResult out;
    if (path.empty())
        return out;

    base::ReadFileResult file = base::read_file(path, "loader.io");
    if (file.error.has_value()) {
        out.error = std::move(file.error);
        return out;
    }
    base::Json::ParseResult parsed = base::Json::parse(file.bytes, path);
    if (parsed.error.has_value()) {
        out.error = base::to_error(*parsed.error);
        return out;
    }
    const base::Json* components =
        parsed.value.is_object() ? parsed.value.find("components") : nullptr;
    if (components == nullptr || !components->is_array()) {
        out.error =
            base::Error{.code = "loader.bad_value",
                        .message = path + ": component manifest needs a 'components' array"};
        return out;
    }
    for (const base::Json& entry : components->elements()) {
        const base::Json* name = entry.is_object() ? entry.find("name") : nullptr;
        if (name != nullptr && name->is_string())
            out.vocab.extracted.push_back(name->as_string());
    }
    return out;
}

} // namespace midday::loader
