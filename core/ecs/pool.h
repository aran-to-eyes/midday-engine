// core/ecs/pool.h — per-component-type storage: a SparseSet (membership +
// dense order + active bits) plus a packed value array parallel to dense.
//
// Contract:
//   * Component values live at their row's dense position; swap-and-pop on
//     erase mirrors the SparseSet exactly, so &data()[find(e)] is the value.
//   * Toggling a row active/inactive is a bit write in the SparseSet — the
//     value array is NEVER touched (asserted by ecs.pool tests via address
//     comparison).
//   * Component types must be nothrow-movable: erase runs on structural
//     paths where exceptions are banned.
//   * Rows are created at entity spawn/setup and die ONLY at despawn — the
//     statechart model toggles activity, it never removes components
//     mid-lifetime (spec section 4.1). There is deliberately no public
//     per-component remove.

#pragma once

#include "core/base/name.h"
#include "core/ecs/sparse_set.h"

#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace midday::ecs {

// The type-erased face: what World needs for uniform structural walks
// (despawn) without knowing T. Iteration never goes through this interface —
// views hold typed Pool<T> pointers (no virtual calls on the hot path).
class PoolBase {
public:
    explicit PoolBase(base::Name name) : name_(name) {}

    PoolBase(const PoolBase&) = delete;
    PoolBase& operator=(const PoolBase&) = delete;
    PoolBase(PoolBase&&) = delete;
    PoolBase& operator=(PoolBase&&) = delete;
    virtual ~PoolBase() = default;

    [[nodiscard]] base::Name name() const { return name_; }

    [[nodiscard]] const SparseSet& set() const { return set_; }

    // Pre: dense_pos < set().size(). Zero memory movement (bit write only).
    void set_row_active(std::uint32_t dense_pos, bool active) {
        set_.set_active(dense_pos, active);
    }

    // Drops the row for entity_index if present (despawn path). Returns
    // whether a row existed.
    virtual bool erase(std::uint32_t entity_index) = 0;

protected:
    SparseSet set_;

private:
    base::Name name_;
};

template <typename T> class Pool final : public PoolBase {
    static_assert(std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>,
                  "component types must be nothrow-movable: pool maintenance runs on "
                  "structural paths where exceptions are banned");

public:
    using PoolBase::PoolBase;

    // Pre: no row for entity_index yet (World checks and lifts violations
    // into structured Errors).
    T& emplace(std::uint32_t entity_index, T value, bool active) {
        const std::uint32_t pos = set_.insert(entity_index, active);
        data_.push_back(std::move(value));
        return data_[pos];
    }

    bool erase(std::uint32_t entity_index) override {
        if (!set_.contains(entity_index))
            return false;
        const SparseSet::RemoveResult removed = set_.remove(entity_index);
        if (removed.moved_last)
            data_[removed.dense_pos] = std::move(data_.back());
        data_.pop_back();
        return true;
    }

    [[nodiscard]] T* try_get(std::uint32_t entity_index) {
        const std::uint32_t pos = set_.find(entity_index);
        return pos == kNpos ? nullptr : &data_[pos];
    }

    [[nodiscard]] const T* try_get(std::uint32_t entity_index) const {
        const std::uint32_t pos = set_.find(entity_index);
        return pos == kNpos ? nullptr : &data_[pos];
    }

    // Pre: dense_pos < set().size().
    [[nodiscard]] T& at_dense(std::uint32_t dense_pos) { return data_[dense_pos]; }

    [[nodiscard]] const T& at_dense(std::uint32_t dense_pos) const { return data_[dense_pos]; }

    // The packed value array — address anchor for the zero-move guarantees.
    [[nodiscard]] const std::vector<T>& data() const { return data_; }

private:
    std::vector<T> data_;
};

} // namespace midday::ecs
