// core/base/arena.h — the frame/arena allocator: bump allocation with bulk
// reset, the engine's per-frame scratch memory.
//
// Contract:
//   * Layout is a pure function of the allocation sequence: offsets are
//     aligned, never pointers, so padding — and therefore bytes_used() and
//     block layout — is identical across runs (determinism contract, spec
//     section 4.3). Supported alignment is capped at alignof(max_align_t);
//     wider (e.g. SIMD/cacheline) alignment lands with its first consumer.
//   * reset() retains every block: a steady-state frame performs zero heap
//     allocations after warm-up.
//   * No per-object free, no destructors at reset — create()/allocate_span()
//     require trivially destructible types by design.
//   * Single-threaded by contract (one arena per owner); the job system
//     (m2-jobs) gives workers their own arenas rather than sharing one.
//
// Consumers: none yet — the tick loop and loader shipped without it. It awaits
// m2-jobs frame packets, where each worker gets its own arena for per-frame
// scratch (the single-threaded-per-owner contract above is why).

#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace midday::base {

class Arena {
public:
    static constexpr std::size_t kDefaultBlockSize = std::size_t{64} * 1024;

    explicit Arena(std::size_t block_size = kDefaultBlockSize);

    ~Arena() = default;
    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;
    Arena(Arena&&) = delete;
    Arena& operator=(Arena&&) = delete;

    // Raw allocation. Pre: size > 0; align is a power of two <= alignof(max_align_t).
    // The returned memory is uninitialized.
    void* allocate(std::size_t size, std::size_t align = alignof(std::max_align_t));

    // Construct a T in place. Frame semantics: no destructor ever runs.
    template <typename T, typename... Args> T* create(Args&&... args) {
        static_assert(std::is_trivially_destructible_v<T>,
                      "frame arenas never run destructors (contract above)");
        return ::new (allocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
    }

    // A contiguous span of `count` value-initialized Ts (zeroed for scalars —
    // never garbage; deterministic content, not just deterministic layout).
    template <typename T> std::span<T> allocate_span(std::size_t count) {
        static_assert(std::is_trivially_destructible_v<T>,
                      "frame arenas never run destructors (contract above)");
        T* data = static_cast<T*>(allocate(sizeof(T) * count, alignof(T)));
        std::uninitialized_value_construct_n(data, count);
        return {data, count};
    }

    // Start the next frame: every block is kept, cursors rewind to zero.
    void reset();

    // Observables (the determinism tests diff these across runs).
    [[nodiscard]] std::size_t bytes_used() const { return bytes_used_; }

    [[nodiscard]] std::size_t block_count() const { return blocks_.size(); }

private:
    struct Block {
        std::unique_ptr<std::byte[]> data;
        std::size_t capacity = 0;
        std::size_t used = 0;
    };

    Block& grow(std::size_t min_capacity);

    std::vector<Block> blocks_;
    std::size_t block_size_;
    std::size_t active_ = 0; // blocks_[active_] is the current bump target
    std::size_t bytes_used_ = 0;
};

} // namespace midday::base
