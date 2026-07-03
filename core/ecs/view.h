// core/ecs/view.h — non-owning multi-component iteration (EnTT's cheap-view
// side of the pay-per-use split; the opt-in owning groups that physically
// pack pools are designed in core/ecs/group.h and land at m2-jobs).
//
// Semantics (spec section 4.1 query rules):
//   * Default iteration visits rows where EVERY queried component is ACTIVE
//     — inactive state-owned components are invisible to systems.
//   * include_inactive() is the explicit opt-in that visits all rows with
//     the components present, regardless of activity.
//
// Cost model (the hottest loop in the engine):
//   * Driver = the smallest queried pool (first-smallest on ties —
//     deterministic). Its active bits are WORD-SCANNED: one 64-bit load per
//     64 rows, one countr_zero per visited row. The active test is never a
//     per-row function call.
//   * Each other pool costs one paged sparse lookup (shift + mask + two
//     loads) and one active-bit test per driver candidate. Single-component
//     views therefore run the pure word-scan with zero lookups.
//   * No virtual calls, no std::function: the visit callback is a template
//     parameter, inlined into the scan.
//
// Iteration safety: each() holds the World's iteration scope — structural
// mutation (spawn/despawn/emplace/flush) inside the callback is refused with
// a structured Error; the deferred queue (World::queue_*) is the sanctioned
// path. Active toggles ARE allowed mid-iteration (they move nothing); a
// toggle lands in the current scan only if its 64-row word has not been
// loaded yet — deterministic for a deterministic script, either way.

#pragma once

#include "core/ecs/entity.h"
#include "core/ecs/pool.h"
#include "core/ecs/sparse_set.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <utility>
#include <vector>

namespace midday::ecs {

namespace detail {

// Word-scan the active bits of [0, count) dense rows; visit(dense_pos) for
// each set bit. include_inactive substitutes a full word (branch per word,
// not per row). Tail bits beyond count are masked off.
template <typename Fn>
void scan_active_rows(const std::vector<std::uint64_t>& words,
                      std::uint32_t count,
                      bool include_inactive,
                      Fn&& visit) {
    for (std::uint32_t base = 0; base < count; base += 64u) {
        std::uint64_t word = include_inactive ? ~std::uint64_t{0} : words[base >> 6u];
        const std::uint32_t remain = count - base;
        if (remain < 64u)
            word &= (std::uint64_t{1} << remain) - 1;
        while (word != 0) {
            const auto bit = static_cast<std::uint32_t>(std::countr_zero(word));
            visit(base + bit);
            word &= word - 1;
        }
    }
}

} // namespace detail

// RAII marker for "a view is running": while the depth is nonzero, direct
// structural mutation on the owning World is a structured error.
class IterationScope {
public:
    explicit IterationScope(std::uint32_t& depth) : depth_(&depth) { ++*depth_; }

    IterationScope(const IterationScope&) = delete;
    IterationScope& operator=(const IterationScope&) = delete;
    IterationScope(IterationScope&&) = delete;
    IterationScope& operator=(IterationScope&&) = delete;

    ~IterationScope() { --*depth_; }

private:
    std::uint32_t* depth_;
};

template <typename... Ts> class View {
    static_assert(sizeof...(Ts) >= 1, "a view queries at least one component type");

public:
    View(const EntityTable& table, std::uint32_t& iteration_depth, Pool<Ts>&... pools)
        : table_(&table), iteration_depth_(&iteration_depth), pools_(&pools...) {}

    // Opt-in: visit rows whose components are present but inactive too
    // (spec section 4.1 — querying inactive components is explicit).
    View& include_inactive() {
        include_inactive_ = true;
        return *this;
    }

    // Visits fn(EntityRef, Ts&...) for every matching entity, in the
    // driver pool's dense order (deterministic, toggle-independent).
    template <typename Fn> void each(Fn&& fn) {
        IterationScope scope(*iteration_depth_);
        each_from(
            fn, driver_index(std::index_sequence_for<Ts...>{}), std::index_sequence_for<Ts...>{});
    }

private:
    template <std::size_t... Is>
    [[nodiscard]] std::size_t driver_index(std::index_sequence<Is...>) const {
        const std::array<std::uint32_t, sizeof...(Ts)> sizes = {
            std::get<Is>(pools_)->set().size()...};
        std::size_t best = 0;
        for (std::size_t i = 1; i < sizes.size(); ++i) {
            if (sizes[i] < sizes[best])
                best = i; // first-smallest: deterministic tie-break
        }
        return best;
    }

    template <typename Fn, std::size_t... Is>
    void each_from(Fn& fn, std::size_t driver, std::index_sequence<Is...> seq) {
        const bool dispatched = ((Is == driver && (each_driven<Is>(fn, seq), true)) || ...);
        (void)dispatched;
    }

    template <std::size_t I, typename Fn, std::size_t... Is>
    void each_driven(Fn& fn, std::index_sequence<Is...>) {
        const SparseSet& driver = std::get<I>(pools_)->set();
        const std::vector<std::uint32_t>& dense = driver.dense();
        detail::scan_active_rows(
            driver.active_words(), driver.size(), include_inactive_, [&](std::uint32_t dpos) {
                const std::uint32_t entity_index = dense[dpos];
                std::array<std::uint32_t, sizeof...(Ts)> rows{};
                rows[I] = dpos;
                if ((join<Is, I>(entity_index, rows) && ...))
                    fn(table_->ref_of(entity_index), std::get<Is>(pools_)->at_dense(rows[Is])...);
            });
    }

    // Membership + activity test against a non-driver pool; records the
    // row's dense position on success. The J == I case is the driver itself
    // — already matched by the word scan.
    template <std::size_t J, std::size_t I>
    [[nodiscard]] bool join(std::uint32_t entity_index,
                            std::array<std::uint32_t, sizeof...(Ts)>& rows) const {
        if constexpr (J == I) {
            (void)entity_index;
            (void)rows;
            return true;
        } else {
            const SparseSet& set = std::get<J>(pools_)->set();
            const std::uint32_t pos = set.find(entity_index);
            if (pos == kNpos)
                return false;
            if (!include_inactive_ && !set.is_active(pos))
                return false;
            rows[J] = pos;
            return true;
        }
    }

    const EntityTable* table_;
    std::uint32_t* iteration_depth_;
    std::tuple<Pool<Ts>*...> pools_;
    bool include_inactive_ = false;
};

} // namespace midday::ecs
