// core/ecs/entity.h — generational entity handles and the slot table behind
// them (spec section 4.1: entities are IDs; components live in tables).
//
// Contract:
//   * EntityRef = 32-bit slot index + 32-bit generation. A ref names ONE
//     incarnation of a slot: despawn bumps the slot's generation, so every
//     outstanding ref to the dead entity mismatches forever after. Stale
//     handles are DETECTED (structured Error at the World API, core/ecs/
//     world.h) — never UB, never an abort.
//   * Slot reuse is LIFO (free-list stack): allocation state is a pure
//     function of the spawn/despawn sequence, so identical scripts produce
//     identical index/generation assignments across runs and platforms —
//     part of the deterministic-iteration contract (spec section 4.3).
//   * Generations wrap at 2^32 reuses of a single slot. Accepted: a slot
//     despawned 4 billion times inside one world lifetime is outside the
//     engine's operating envelope (and a wrapped collision still names a
//     live entity, not freed memory).
//   * Slots are FREE -> (PENDING ->) ALIVE -> FREE. PENDING is the deferred-
//     spawn window: World::queue_spawn reserves the handle immediately (so
//     systems can wire references mid-iteration) but the entity only becomes
//     visible to queries at the structural flush (core/ecs/structural_queue.h).

#pragma once

#include <cassert>
#include <cstdint>
#include <vector>

namespace midday::ecs {

inline constexpr std::uint32_t kNullEntityIndex = 0xFFFFFFFFu;

struct EntityRef {
    std::uint32_t index = kNullEntityIndex;
    std::uint32_t generation = 0;

    [[nodiscard]] bool is_null() const { return index == kNullEntityIndex; }

    // Journal/serialization form: generation in the high 32 bits.
    [[nodiscard]] std::uint64_t to_bits() const {
        return (static_cast<std::uint64_t>(generation) << 32u) | index;
    }

    static EntityRef from_bits(std::uint64_t bits) {
        return EntityRef{static_cast<std::uint32_t>(bits & 0xFFFFFFFFu),
                         static_cast<std::uint32_t>(bits >> 32u)};
    }

    friend bool operator==(const EntityRef&, const EntityRef&) = default;
};

enum class SlotState : std::uint8_t {
    kFree,
    kPending, // reserved by queue_spawn; becomes kAlive at the structural flush
    kAlive,
};

// The slot table: generations, states, and the LIFO free list. Internal
// preconditions are asserts — World validates every EXTERNAL handle first
// and lifts violations into structured Errors.
class EntityTable {
public:
    // Reserves a slot in `initial` state (kAlive for direct spawn, kPending
    // for a queued one) and returns its handle.
    [[nodiscard]] EntityRef allocate(SlotState initial) {
        assert(initial != SlotState::kFree);
        std::uint32_t index = 0;
        if (!free_.empty()) {
            index = free_.back();
            free_.pop_back();
        } else {
            index = static_cast<std::uint32_t>(generation_.size());
            generation_.push_back(0);
            state_.push_back(SlotState::kFree);
        }
        state_[index] = initial;
        if (initial == SlotState::kAlive)
            ++alive_count_;
        return EntityRef{index, generation_[index]};
    }

    // True iff `ref` names the CURRENT incarnation of its slot (alive or
    // pending). A freed slot's stored generation is always one past the last
    // handed-out ref, so a matching generation implies a non-free slot.
    [[nodiscard]] bool is_current(EntityRef ref) const {
        return ref.index < generation_.size() && generation_[ref.index] == ref.generation &&
               state_[ref.index] != SlotState::kFree;
    }

    [[nodiscard]] bool is_alive(EntityRef ref) const {
        return ref.index < generation_.size() && generation_[ref.index] == ref.generation &&
               state_[ref.index] == SlotState::kAlive;
    }

    [[nodiscard]] bool is_pending(EntityRef ref) const {
        return ref.index < generation_.size() && generation_[ref.index] == ref.generation &&
               state_[ref.index] == SlotState::kPending;
    }

    // Pre: is_pending(ref). The structural flush's spawn application.
    void make_alive(EntityRef ref) {
        assert(is_pending(ref));
        state_[ref.index] = SlotState::kAlive;
        ++alive_count_;
    }

    // Pre: is_alive(ref). Bumps the generation (staling every outstanding
    // ref) and recycles the slot.
    void release(EntityRef ref) {
        assert(is_alive(ref));
        ++generation_[ref.index]; // wraps after 2^32 reuses — accepted, see header
        state_[ref.index] = SlotState::kFree;
        free_.push_back(ref.index);
        --alive_count_;
    }

    // Pre: index < slot_count(). The current generation of a slot — used by
    // views to reconstruct EntityRefs from dense entity indices.
    [[nodiscard]] std::uint32_t generation_of(std::uint32_t index) const {
        assert(index < generation_.size());
        return generation_[index];
    }

    // Pre: index < slot_count(). The handle of the slot's CURRENT incarnation.
    [[nodiscard]] EntityRef ref_of(std::uint32_t index) const {
        return EntityRef{index, generation_of(index)};
    }

    [[nodiscard]] std::uint32_t slot_count() const {
        return static_cast<std::uint32_t>(generation_.size());
    }

    [[nodiscard]] std::uint32_t alive_count() const { return alive_count_; }

private:
    std::vector<std::uint32_t> generation_;
    std::vector<SlotState> state_;
    std::vector<std::uint32_t> free_; // LIFO: deterministic reuse order
    std::uint32_t alive_count_ = 0;
};

} // namespace midday::ecs
