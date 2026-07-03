// core/ecs/sparse_set.h — the paged sparse set: THE storage primitive of the
// ECS (EnTT's proven shape — see ENGINE_ARCHITECTURE_RESEARCH.md section 3).
// Sparse-set storage was chosen over archetypes because the statechart
// entity model toggles component sets active/inactive every state change
// (spec section 4.1): a toggle here is one bit write; in archetype storage
// it would be a table move.
//
// Layout:
//   * sparse: pages of entity-index -> dense-position, allocated on first
//     touch. Lookup is shift + mask + two loads; untouched regions cost
//     nothing.
//   * dense:  packed entity indices — THE iteration order. Insert appends;
//     remove swap-and-pops. Both are pure functions of the operation
//     sequence, so iteration order is deterministic across runs and
//     platforms (spec section 4.3) — and toggling active bits never touches
//     dense at all, so order is independent of toggle history by
//     construction.
//   * active: one bit per dense row, stored as 64-bit words parallel to
//     dense. Toggling activity is a single bit write — ZERO memory movement,
//     no reallocation, no reordering. Default iteration word-scans these
//     bits (core/ecs/view.h): one load per 64 rows, one countr_zero per
//     active row — the active test never becomes a per-row call.
//
// Page size: 4096 entries (16 KiB of uint32 per page — EnTT's default).
//   * 2^12 keeps page addressing a shift/mask (index >> 12, index & 0xFFF).
//   * 16 KiB = four 4 KiB OS pages: big enough that the entity allocator's
//     dense-from-zero indices land almost entirely in page 0 for typical
//     scenes (one indirection that stays cache-hot), small enough that a
//     pathological isolated index costs at most 16 KiB of tombstones.

#pragma once

#include <array>
#include <cassert>
#include <cstdint>
#include <memory>
#include <vector>

namespace midday::ecs {

inline constexpr std::uint32_t kSparsePageShift = 12u;
inline constexpr std::uint32_t kSparsePageSize = 1u << kSparsePageShift; // 4096 entries
inline constexpr std::uint32_t kNpos = 0xFFFFFFFFu;

class SparseSet {
public:
    struct RemoveResult {
        std::uint32_t dense_pos = 0; // the vacated position
        bool moved_last = false;     // true if the last row was swapped into it
    };

    // Pre: !contains(entity_index). Appends a dense row and returns its
    // position; the row's active bit starts as `active`.
    std::uint32_t insert(std::uint32_t entity_index, bool active) {
        const auto pos = static_cast<std::uint32_t>(dense_.size());
        std::uint32_t& slot = sparse_slot(entity_index);
        assert(slot == kNpos);
        slot = pos;
        dense_.push_back(entity_index);
        if ((pos >> 6u) >= active_.size())
            active_.push_back(0);
        write_bit(pos, active);
        return pos;
    }

    // Pre: contains(entity_index). Swap-and-pop; the moved row's active bit
    // moves with it. Bits beyond size() are dead — insert overwrites them.
    RemoveResult remove(std::uint32_t entity_index) {
        const std::uint32_t pos = find(entity_index);
        assert(pos != kNpos);
        const auto last = static_cast<std::uint32_t>(dense_.size()) - 1;
        const bool moved = pos != last;
        if (moved) {
            const std::uint32_t moved_entity = dense_[last];
            dense_[pos] = moved_entity;
            sparse_slot(moved_entity) = pos;
            write_bit(pos, is_active(last));
        }
        sparse_slot(entity_index) = kNpos;
        dense_.pop_back();
        return RemoveResult{pos, moved};
    }

    // Dense position of entity_index, kNpos when absent.
    [[nodiscard]] std::uint32_t find(std::uint32_t entity_index) const {
        const std::uint32_t page = entity_index >> kSparsePageShift;
        if (page >= pages_.size() || pages_[page] == nullptr)
            return kNpos;
        return (*pages_[page])[entity_index & (kSparsePageSize - 1)];
    }

    [[nodiscard]] bool contains(std::uint32_t entity_index) const {
        return find(entity_index) != kNpos;
    }

    [[nodiscard]] std::uint32_t size() const { return static_cast<std::uint32_t>(dense_.size()); }

    [[nodiscard]] bool empty() const { return dense_.empty(); }

    // Packed entity indices in iteration order.
    [[nodiscard]] const std::vector<std::uint32_t>& dense() const { return dense_; }

    // Pre: dense_pos < size(). One bit write — never moves memory.
    void set_active(std::uint32_t dense_pos, bool active) {
        assert(dense_pos < dense_.size());
        write_bit(dense_pos, active);
    }

    [[nodiscard]] bool is_active(std::uint32_t dense_pos) const {
        assert(dense_pos < dense_.size());
        return (active_[dense_pos >> 6u] & (std::uint64_t{1} << (dense_pos & 63u))) != 0;
    }

    // The raw bit words for word-scan iteration (core/ecs/view.h). Bits at
    // positions >= size() are meaningless; scanners mask the tail.
    [[nodiscard]] const std::vector<std::uint64_t>& active_words() const { return active_; }

private:
    using Page = std::array<std::uint32_t, kSparsePageSize>;

    // The sparse slot for entity_index, allocating (tombstone-filled) pages
    // on first touch.
    std::uint32_t& sparse_slot(std::uint32_t entity_index) {
        const std::uint32_t page = entity_index >> kSparsePageShift;
        if (page >= pages_.size())
            pages_.resize(page + 1);
        if (pages_[page] == nullptr) {
            pages_[page] = std::make_unique<Page>();
            pages_[page]->fill(kNpos);
        }
        return (*pages_[page])[entity_index & (kSparsePageSize - 1)];
    }

    void write_bit(std::uint32_t pos, bool value) {
        const std::uint64_t mask = std::uint64_t{1} << (pos & 63u);
        if (value) {
            active_[pos >> 6u] |= mask;
        } else {
            active_[pos >> 6u] &= ~mask;
        }
    }

    std::vector<std::unique_ptr<Page>> pages_;
    std::vector<std::uint32_t> dense_;
    std::vector<std::uint64_t> active_;
};

} // namespace midday::ecs
