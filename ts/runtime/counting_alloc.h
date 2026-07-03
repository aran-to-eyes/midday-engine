// ts/runtime/counting_alloc.h — the counting allocator behind
// ScriptRuntime::alloc_bytes() (m0-batch-bindings GC-churn budget,
// D-BUILD-071): the default C allocator plus a cumulative allocated-bytes
// meter. Behavior is byte-identical to QuickJS's own defaults — same
// allocator, same usable-size probe — so counting changes nothing
// observable but the counter. Frees never decrement: the meter measures
// CHURN, not residency, and a steady-state tick that reads zero performed
// literally no heap allocation (no GC can ever trigger).
//
// INTERNAL: speaks quickjs.h, so only ts/runtime .cpp files may include it
// (script_runtime.cpp does); it must never leak into a public header.

#pragma once

#include <cstdint>
#include <cstdlib>
#include <quickjs.h>

#if defined(__APPLE__)
#include <malloc/malloc.h> // malloc_size
#elif defined(_WIN32)
#include <malloc.h> // _msize
#else
#include <malloc.h> // malloc_usable_size (glibc)
#endif

namespace midday::script::detail {

struct AllocMeter {
    std::uint64_t bytes = 0;
};

inline std::size_t counted_usable_size(const void* ptr) {
#if defined(__APPLE__)
    return malloc_size(ptr);
#elif defined(_WIN32)
    return _msize(const_cast<void*>(ptr));
#else
    return malloc_usable_size(const_cast<void*>(ptr));
#endif
}

inline void* counted_calloc(void* opaque, std::size_t count, std::size_t size) {
    static_cast<AllocMeter*>(opaque)->bytes += static_cast<std::uint64_t>(count) * size;
    return std::calloc(count, size);
}

inline void* counted_malloc(void* opaque, std::size_t size) {
    static_cast<AllocMeter*>(opaque)->bytes += size;
    return std::malloc(size);
}

inline void counted_free(void* /*opaque*/, void* ptr) {
    std::free(ptr);
}

inline void* counted_realloc(void* opaque, void* ptr, std::size_t size) {
    if (ptr == nullptr)
        return counted_malloc(opaque, size);
    if (const std::size_t old_usable = counted_usable_size(ptr); size > old_usable)
        static_cast<AllocMeter*>(opaque)->bytes += size - old_usable;
    return std::realloc(ptr, size);
}

inline constexpr JSMallocFunctions kCountingAlloc = {
    &counted_calloc, &counted_malloc, &counted_free, &counted_realloc, &counted_usable_size};

} // namespace midday::script::detail
