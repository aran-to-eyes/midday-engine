#include "core/base/arena.h"

#include <cassert>

namespace midday::base {

Arena::Arena(std::size_t block_size) : block_size_(block_size) {
    assert(block_size_ > 0);
}

void* Arena::allocate(std::size_t size, std::size_t align) {
    assert(size > 0);
    assert(align > 0 && (align & (align - 1)) == 0 && "alignment must be a power of two");
    assert(align <= alignof(std::max_align_t) &&
           "wider alignment lands with its first consumer (contract in arena.h)");

    // Find the first block from the active cursor that fits the aligned
    // request; offsets are aligned (never pointers) so layout is a pure
    // function of the allocation sequence.
    for (; active_ < blocks_.size(); ++active_) {
        Block& block = blocks_[active_];
        const std::size_t offset = (block.used + align - 1) & ~(align - 1);
        if (offset + size <= block.capacity) {
            bytes_used_ += (offset + size) - block.used; // padding + payload
            block.used = offset + size;
            return block.data.get() + offset;
        }
    }

    Block& block = grow(size);
    block.used = size; // fresh block: offset 0 is max_align_t-aligned by new[]
    bytes_used_ += size;
    return block.data.get();
}

Arena::Block& Arena::grow(std::size_t min_capacity) {
    Block block;
    block.capacity = min_capacity > block_size_ ? min_capacity : block_size_;
    block.data = std::make_unique<std::byte[]>(block.capacity);
    blocks_.push_back(std::move(block));
    active_ = blocks_.size() - 1;
    return blocks_.back();
}

void Arena::reset() {
    for (Block& block : blocks_)
        block.used = 0;
    active_ = 0;
    bytes_used_ = 0;
}

} // namespace midday::base
