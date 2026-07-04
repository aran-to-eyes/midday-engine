// core/rhi/handle.h — generational GPU resource handles (m0-rhi-vulkan).
// The EntityRef discipline (core/ecs/entity.h) applied to the RHI seam:
//
//   * Handle<Tag> = 32-bit slot index + 32-bit generation. A handle names ONE
//     incarnation of a slot: destroy bumps the generation, so every
//     outstanding handle to the dead resource mismatches forever after.
//     Stale handles are DETECTED (structured Error "rhi.stale_handle" at the
//     device API) — never UB, never a backend crash.
//   * Tags make handles TYPED: a BufferHandle cannot be passed where a
//     TextureHandle is expected — misuse is a compile error, not a runtime
//     lookup miss.
//   * Slot reuse is LIFO (free-list stack): handle assignment is a pure
//     function of the create/destroy sequence — same operation script, same
//     indices and generations, on every backend and platform.
//
// HandlePool<Tag, T> is the slot table every backend uses for its
// bookkeeping, so the handle semantics above are shared by construction
// (NullDevice, Vulkan, Metal), not re-implemented per backend.

#pragma once

#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

namespace midday::rhi {

inline constexpr std::uint32_t kNullHandleIndex = 0xFFFFFFFFu;

template <typename Tag> struct Handle {
    std::uint32_t index = kNullHandleIndex;
    std::uint32_t generation = 0;

    [[nodiscard]] bool is_null() const { return index == kNullHandleIndex; }

    // Journal/JSON form: generation in the high 32 bits (EntityRef::to_bits).
    [[nodiscard]] std::uint64_t to_bits() const {
        return (static_cast<std::uint64_t>(generation) << 32u) | index;
    }

    static Handle from_bits(std::uint64_t bits) {
        return Handle{static_cast<std::uint32_t>(bits & 0xFFFFFFFFu),
                      static_cast<std::uint32_t>(bits >> 32u)};
    }

    friend bool operator==(const Handle&, const Handle&) = default;
};

// The six seam resource kinds. Tags are declared inline; no definition is
// ever needed — they exist only to make the handle types distinct.
using BufferHandle = Handle<struct BufferHandleTag>;
using TextureHandle = Handle<struct TextureHandleTag>;
using SamplerHandle = Handle<struct SamplerHandleTag>;
using ShaderHandle = Handle<struct ShaderHandleTag>;
using PipelineHandle = Handle<struct PipelineHandleTag>;
using CommandListHandle = Handle<struct CommandListHandleTag>;

// Backend bookkeeping: a generational slot table storing one T per live
// handle. Internal preconditions are asserts; devices validate every
// EXTERNAL handle through get()/is_current and lift misses into structured
// Errors (device.h).
template <typename Tag, typename T> class HandlePool {
public:
    [[nodiscard]] Handle<Tag> create(T value) {
        std::uint32_t index = 0;
        if (!free_.empty()) {
            index = free_.back();
            free_.pop_back();
            slots_[index].value = std::move(value);
            slots_[index].live = true;
        } else {
            index = static_cast<std::uint32_t>(slots_.size());
            slots_.push_back(Slot{std::move(value), 0, true});
        }
        ++live_count_;
        return Handle<Tag>{index, slots_[index].generation};
    }

    [[nodiscard]] bool is_current(Handle<Tag> handle) const {
        return handle.index < slots_.size() &&
               slots_[handle.index].generation == handle.generation && slots_[handle.index].live;
    }

    // The live resource named by `handle`, or nullptr (null/stale/destroyed).
    [[nodiscard]] T* get(Handle<Tag> handle) {
        return is_current(handle) ? &slots_[handle.index].value : nullptr;
    }

    [[nodiscard]] const T* get(Handle<Tag> handle) const {
        return is_current(handle) ? &slots_[handle.index].value : nullptr;
    }

    // Pre: is_current(handle). Bumps the generation (staling every
    // outstanding handle) and recycles the slot. Returns the stored value so
    // the backend can release the underlying GPU objects.
    T release(Handle<Tag> handle) {
        assert(is_current(handle));
        Slot& slot = slots_[handle.index];
        ++slot.generation; // wraps after 2^32 reuses of one slot — accepted (EntityRef)
        slot.live = false;
        free_.push_back(handle.index);
        --live_count_;
        return std::exchange(slot.value, T{});
    }

    [[nodiscard]] std::uint32_t live_count() const { return live_count_; }

    [[nodiscard]] std::uint32_t slot_count() const {
        return static_cast<std::uint32_t>(slots_.size());
    }

    // Deterministic teardown sweep (device destruction): visits every live
    // value in ascending slot order.
    template <typename Fn> void for_each_live(Fn&& fn) {
        for (Slot& slot : slots_)
            if (slot.live)
                fn(slot.value);
    }

private:
    struct Slot {
        T value{};
        std::uint32_t generation = 0;
        bool live = false;
    };

    std::vector<Slot> slots_;
    std::vector<std::uint32_t> free_; // LIFO: deterministic reuse order
    std::uint32_t live_count_ = 0;
};

} // namespace midday::rhi
