// core/ecs/world.h — the ECS facade: entity lifetimes, typed pools, views,
// and the deferred structural queue, behind stale-safe generational handles.
//
// Error policy (two clean classes):
//   * HANDLE problems are STRUCTURED ERRORS — stale refs, pending refs,
//     missing/duplicate components, structural mutation during iteration.
//     Codes: ecs.stale_handle, ecs.entity_pending, ecs.missing_component,
//     ecs.duplicate_component, ecs.structural_during_iteration,
//     ecs.reparent_unhandled. Never UB, never abort, never an exception.
//   * REGISTRATION problems are LOUD ABORTS — registering a component type
//     twice, or touching a pool for an unregistered type. Registration is
//     the boot path, single-threaded by contract, exactly like the
//     reflection registry it bridges into (D-BUILD-023).
//
// Registration bridge: register_component<T>(ClassDesc) records the class
// in the bound reflect::Registry (so engine_api.json, codegen, and agents
// see every component class) AND creates the typed pool, keyed by the same
// Name. One call, one identity, no codegen yet — the bridge stays thin.
//
// Determinism: every observable order is a pure function of the operation
// sequence — LIFO slot reuse (entity.h), swap-and-pop dense order
// (sparse_set.h), per-World registration order for all-pool walks (never
// component_type_id order, which is process-global first-touch), and queue
// order for the structural flush. ecs.determinism pins this with XXH3
// visit-order hashes across two independent runs.

#pragma once

#include "core/base/error.h"
#include "core/ecs/component.h"
#include "core/ecs/entity.h"
#include "core/ecs/group.h" // design-only until m2-jobs; see its header
#include "core/ecs/pool.h"
#include "core/ecs/structural_queue.h"
#include "core/ecs/view.h"
#include "core/reflect/registry.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace midday::ecs {

namespace detail {

// Structured-error constructors (world.cpp) — kept out of the templates.
[[nodiscard]] base::Error structural_locked_error(std::string_view operation);
[[nodiscard]] base::Error missing_component_error(base::Name component, EntityRef ref);
[[nodiscard]] base::Error duplicate_component_error(base::Name component, EntityRef ref);
// Loud aborts for registration misuse (boot-path invariants).
[[noreturn]] void fail_component_reregistered(base::Name name);
[[noreturn]] void fail_unregistered_pool();

} // namespace detail

class World {
public:
    // The registry outlives the World; every component class registers
    // through it so downstream reflection consumers see the full set.
    explicit World(reflect::Registry& registry) : registry_(&registry) {}

    World(const World&) = delete;
    World& operator=(const World&) = delete;
    World(World&&) = delete;
    World& operator=(World&&) = delete;
    ~World() = default;

    // ---- registration bridge (boot path; aborts loudly on misuse) --------
    template <typename T> void register_component(reflect::ClassDesc desc) {
        const std::uint32_t id = component_type_id<T>();
        if (id < pools_.size() && pools_[id] != nullptr)
            detail::fail_component_reregistered(desc.name);
        const base::Name name = desc.name;
        registry_->add_class(std::move(desc)); // validates + aborts on duplicates
        if (pools_.size() <= id)
            pools_.resize(id + 1);
        auto pool = std::make_unique<Pool<T>>(name);
        pool_order_.push_back(pool.get());
        pools_[id] = std::move(pool);
    }

    // ---- handles ----------------------------------------------------------
    [[nodiscard]] bool alive(EntityRef ref) const { return table_.is_alive(ref); }

    // nullopt when alive; otherwise the structured refusal (ecs.stale_handle
    // or ecs.entity_pending) every handle-taking mutator returns.
    [[nodiscard]] std::optional<base::Error> check_alive(EntityRef ref) const;

    [[nodiscard]] std::uint32_t alive_count() const { return table_.alive_count(); }

    // ---- direct structural mutation (refused during iteration) -----------
    // Returns the new entity, or the null ref (writing *error if given)
    // when called mid-iteration — use queue_spawn there.
    EntityRef spawn(base::Error* error = nullptr);

    std::optional<base::Error> despawn(EntityRef ref);

    // Attaches a component row (active by default; state-owned components
    // pass their state's activity). Rows persist until despawn — activity
    // toggles, never destruction (spec section 4.1).
    template <typename T>
    std::optional<base::Error> emplace(EntityRef ref, T value, bool active = true) {
        if (iteration_depth_ != 0)
            return detail::structural_locked_error("emplace");
        if (auto error = check_alive(ref))
            return error;
        Pool<T>& pool = pool_ref<T>();
        if (pool.set().contains(ref.index))
            return detail::duplicate_component_error(pool.name(), ref);
        pool.emplace(ref.index, std::move(value), active);
        return std::nullopt;
    }

    // ---- activity (NOT structural: allowed during iteration, moves zero
    // memory — one bit write) ----------------------------------------------
    template <typename T> std::optional<base::Error> set_active(EntityRef ref, bool active) {
        if (auto error = check_alive(ref))
            return error;
        Pool<T>& pool = pool_ref<T>();
        const std::uint32_t pos = pool.set().find(ref.index);
        if (pos == kNpos)
            return detail::missing_component_error(pool.name(), ref);
        pool.set_row_active(pos, active);
        return std::nullopt;
    }

    // nullopt when the handle is not alive or the component is absent.
    template <typename T> [[nodiscard]] std::optional<bool> is_active(EntityRef ref) const {
        if (!table_.is_alive(ref))
            return std::nullopt;
        const Pool<T>& pool = pool_ref<T>();
        const std::uint32_t pos = pool.set().find(ref.index);
        if (pos == kNpos)
            return std::nullopt;
        return pool.set().is_active(pos);
    }

    // ---- component access (hot path: pointer-or-null, no Error built) ----
    template <typename T> [[nodiscard]] T* try_get(EntityRef ref) {
        if (!table_.is_alive(ref))
            return nullptr;
        return pool_ref<T>().try_get(ref.index);
    }

    template <typename T> [[nodiscard]] const T* try_get(EntityRef ref) const {
        if (!table_.is_alive(ref))
            return nullptr;
        return pool_ref<T>().try_get(ref.index);
    }

    template <typename T> [[nodiscard]] bool has(EntityRef ref) const {
        return table_.is_alive(ref) && pool_ref<T>().set().contains(ref.index);
    }

    // Read-only pool inspection (tests pin the zero-move guarantees on the
    // data() addresses).
    template <typename T> [[nodiscard]] const Pool<T>& pool() const { return pool_ref<T>(); }

    // ---- views -------------------------------------------------------------
    // Default: ACTIVE rows of every queried component. Opt-in:
    // world.view<T...>().include_inactive().each(...).
    template <typename... Ts> [[nodiscard]] View<Ts...> view() {
        return View<Ts...>(table_, iteration_depth_, pool_ref<Ts>()...);
    }

    [[nodiscard]] bool iterating() const { return iteration_depth_ != 0; }

    // ---- deferred structural queue (see structural_queue.h) ---------------
    // Reserves the handle now (pending); the entity goes live at the flush.
    EntityRef queue_spawn();
    std::optional<base::Error> queue_despawn(EntityRef ref);
    std::optional<base::Error> queue_reparent(EntityRef child, EntityRef new_parent);

    void set_reparent_handler(ReparentHandler handler) { reparent_handler_ = std::move(handler); }

    [[nodiscard]] std::size_t pending_command_count() const { return commands_.size(); }

    // Applies all queued commands in queue order at ONE deterministic point
    // (the tick loop's structural-apply phase). Refused mid-iteration.
    std::optional<base::Error> flush_structural(FlushStats* stats = nullptr);

private:
    template <typename T> [[nodiscard]] Pool<T>& pool_ref() {
        const std::uint32_t id = component_type_id<T>();
        if (id >= pools_.size() || pools_[id] == nullptr)
            detail::fail_unregistered_pool();
        return static_cast<Pool<T>&>(*pools_[id]);
    }

    template <typename T> [[nodiscard]] const Pool<T>& pool_ref() const {
        const std::uint32_t id = component_type_id<T>();
        if (id >= pools_.size() || pools_[id] == nullptr)
            detail::fail_unregistered_pool();
        return static_cast<const Pool<T>&>(*pools_[id]);
    }

    // Shared by direct despawn and the flush: drop rows from every pool in
    // per-World registration order (deterministic), then release the slot.
    void despawn_now(EntityRef ref);

    reflect::Registry* registry_;
    EntityTable table_;
    std::vector<std::unique_ptr<PoolBase>> pools_; // indexed by component_type_id (gaps null)
    std::vector<PoolBase*> pool_order_;            // registration order: THE all-pool walk order
    std::vector<StructuralCommand> commands_;
    ReparentHandler reparent_handler_;
    std::uint32_t iteration_depth_ = 0;
};

} // namespace midday::ecs
