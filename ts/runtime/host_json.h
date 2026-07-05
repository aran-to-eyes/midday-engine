// ts/runtime/host_json.h — the tiny JSON<->native argument-marshaling
// helpers every script-facing host seat needs (ts/runtime/component_host.cpp,
// ts/runtime/world_host.cpp): an EntityRef crosses the host boundary as two
// numbers (index, generation) in both directions, and a bad-arity/bad-type
// call refuses with the SAME "script.host" shape everywhere. Header-only,
// `inline`, no library of its own — shared to hold the jscpd ratchet rather
// than have each seat re-derive the same four lines.

#pragma once

#include "core/base/error.h"
#include "core/base/json.h"
#include "core/ecs/entity.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace midday::script {

inline std::uint32_t host_as_u32(const base::Json& value) {
    return static_cast<std::uint32_t>(value.as_double());
}

inline ecs::EntityRef
host_entity_arg(const base::Json::Array& args, std::size_t index_pos, std::size_t gen_pos) {
    return ecs::EntityRef{host_as_u32(args[index_pos]), host_as_u32(args[gen_pos])};
}

inline base::Json host_ref_json(ecs::EntityRef ref) {
    base::Json out = base::Json::object();
    out.set("index", static_cast<std::int64_t>(ref.index));
    out.set("generation", static_cast<std::int64_t>(ref.generation));
    return out;
}

inline base::Error host_bad_args(std::string_view fn, std::string_view shape) {
    return base::Error{.code = "script.host",
                       .message = std::string(fn) + " expects (" + std::string(shape) + ")"};
}

} // namespace midday::script
