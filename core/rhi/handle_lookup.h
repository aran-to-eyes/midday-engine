// core/rhi/handle_lookup.h — external-handle validation, ONE implementation
// for every backend (hoisted at m0-rhi-metal, the third consumer). A device
// entry point validates each caller-supplied handle through lookup_handle:
// null and stale misses become the shared structured spellings
// ("rhi.null_handle" / "rhi.stale_handle", types.h list), so the wording and
// details are identical across NullDevice, Vulkan, and Metal by construction
// — the same hoisting story as CommandListState and validate.h.

#pragma once

#include "core/base/error.h"
#include "core/rhi/handle.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace midday::rhi {

// Defined in null_device.cpp (midday_rhi, the seam library every backend
// links) so message wording lives in exactly one translation unit.
[[nodiscard]] base::Error null_handle_error(std::string_view kind);
[[nodiscard]] base::Error stale_handle_error(std::string_view kind, std::uint64_t bits);

// The live resource named by `handle`, or nullptr with `error` set to the
// shared null/stale spelling.
template <typename Tag, typename T>
[[nodiscard]] T* lookup_handle(HandlePool<Tag, T>& pool,
                               Handle<Tag> handle,
                               std::string_view kind,
                               std::optional<base::Error>& error) {
    if (handle.is_null()) {
        error = null_handle_error(kind);
        return nullptr;
    }
    T* value = pool.get(handle);
    if (value == nullptr)
        error = stale_handle_error(kind, handle.to_bits());
    return value;
}

// Validated release for backends whose entries free their GPU objects with
// the slot (RAII vectors, ARC references): null/stale refusal, then recycle.
// Backends with manual release choreography (Vulkan) lookup + release apart.
template <typename Tag, typename T>
[[nodiscard]] std::optional<base::Error>
release_handle(HandlePool<Tag, T>& pool, Handle<Tag> handle, std::string_view kind) {
    std::optional<base::Error> error;
    if (lookup_handle(pool, handle, kind, error) == nullptr)
        return error;
    (void)pool.release(handle);
    return std::nullopt;
}

} // namespace midday::rhi
