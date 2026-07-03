// ts/runtime/batch_views.h — the batch-first script binding layer (spec §7,
// m0-batch-bindings): ECS component pools cross into QuickJS as SoA
// typed-array views, so boundary crossings per tick are O(exposed buffers) —
// a function of the POOL/FIELD count, never of the entity count.
//
// Staged copy, not zero-copy — a decision, not a fallback (D-BUILD-070):
//   * Pools store components AoS (Pool<T>'s packed std::vector<T>); JS typed
//     arrays cannot stride, so per-field views over interleaved memory are
//     impossible without a layout change nothing else wants.
//   * Views expose ACTIVE rows only (spec §4.1 query semantics) — the gather
//     COMPACTS the active join into dense columns; a raw alias would leak
//     inactive rows and dense-order internals.
//   * std::vector reallocation and swap-and-pop make raw aliases lifetime-
//     traps; staging buffers are owned HERE and never dangle.
//   * Writes land at commit() — one deterministic point per phase — so the
//     script always computes over a coherent refresh() snapshot.
//   The staged O(entities) work is plain C++ memcpy/loops; the JS boundary
//   is crossed once per BUFFER, which the counters below prove.
//
// Per-phase protocol (the tick loop's script phase — the bench harness in
// batch_bench.h drives the same cycle): refresh(tick) publishes the join
// into the typed arrays and updates counts -> call_tick(tick) runs the
// script's registered entry -> commit() scatters writable columns back.
//
// Memory safety of exposed buffers (deterministic invalidation):
//   * Buffers are QuickJS ArrayBuffers over C++-owned staging memory. On
//     capacity growth the OLD ArrayBuffer is DETACHED first — stale JS
//     references see length 0 / undefined reads / no-op writes, per the ES
//     detached-buffer rules: deterministic, never dangling. The midday/batch
//     protocol is to re-read view.buffers at each tick entry.
//   * Structural changes between refresh and commit are caught at commit:
//     every gathered entity is re-found in the sparse set; a vanished row
//     refuses the whole commit with bindings.stale_view (no partial writes
//     are lost silently — writes go to re-found positions, so swap-and-pop
//     moves are followed, not corrupted).
//
// Registration is boot-path: expose<T>() columns are validated against the
// reflect ClassDesc registered for the component (field exists, TypeDesc
// matches, writability = !read_only) and mismatches abort loudly
// (D-BUILD-023 discipline). The glue spec these columns implement is the
// generated bindings_spec.json batch_envelope (api/CODEGEN.md) — the one
// source of truth; no hand-written per-API bindings exist.

#pragma once

#include "core/base/error.h"
#include "core/base/name.h"
#include "core/ecs/pool.h"
#include "core/ecs/world.h"
#include "core/math/quat.h"
#include "core/math/vec.h"
#include "core/reflect/registry.h"
#include "ts/runtime/script_runtime.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace midday::script {

// The runtime envelope version scripts receive; midday/batch refuses
// anything else (bindings_spec.json: "refuse envelope_version 0").
inline constexpr std::int64_t kBatchEnvelopeVersion = 1;

// Buffer element kinds — the generated batch_envelope mapping: f32 carries
// float/vec2/vec3/vec4/quat, f64 carries int (2^53-exact, the JSON number
// contract JS already lives under), u8 carries bool.
enum class BatchBuffer : std::uint8_t { kF32, kF64, kU8 };

// Boundary-crossing counters (D-BUILD-071): every typed-array publish and
// read-back is one crossing; ScriptRuntime::host_calls() covers the JSON
// hook seam and call_tick covers the C++ -> JS entry. The bench harness sums
// them into boundary_crossings_per_tick.
struct BatchStats {
    std::uint64_t buffer_refreshes = 0; // column gathers published (refresh)
    std::uint64_t buffer_commits = 0;   // column scatters read back (commit)
    std::uint64_t tick_calls = 0;       // __midday_batch_tick invocations
    std::uint64_t view_rebuilds = 0;    // capacity growths (detach + remap)
};

namespace batch_detail {

// Whole-column kernels: ONE indirect call per column per phase — the loop
// over rows lives inside, mirroring view.h's no-per-row-indirection rule.
using GatherFn = void (*)(const ecs::PoolBase& pool,
                          const std::uint32_t* rows,
                          std::uint32_t count,
                          std::byte* dst);
using ScatterFn = void (*)(ecs::PoolBase& pool,
                           const std::uint32_t* rows,
                           std::uint32_t count,
                           const std::byte* src);

// Member type -> buffer layout + the TypeDesc spelling it must match.
template <typename M> struct ColumnTraits; // unsupported member type = build error

template <> struct ColumnTraits<float> {
    using Elem = float;
    static constexpr BatchBuffer kBuffer = BatchBuffer::kF32;
    static constexpr std::uint32_t kWidth = 1;
    static constexpr std::string_view kSpelling = "float";
};

template <> struct ColumnTraits<bool> {
    using Elem = std::uint8_t;
    static constexpr BatchBuffer kBuffer = BatchBuffer::kU8;
    static constexpr std::uint32_t kWidth = 1;
    static constexpr std::string_view kSpelling = "bool";
};

template <> struct ColumnTraits<std::int64_t> {
    using Elem = double;
    static constexpr BatchBuffer kBuffer = BatchBuffer::kF64;
    static constexpr std::uint32_t kWidth = 1;
    static constexpr std::string_view kSpelling = "int";
};

template <typename V, std::uint32_t W, const char* Spelling> struct FloatTupleTraits {
    using Elem = float;
    static constexpr BatchBuffer kBuffer = BatchBuffer::kF32;
    static constexpr std::uint32_t kWidth = W;
    static constexpr std::string_view kSpelling = Spelling;
    static_assert(sizeof(V) == sizeof(float) * W, "tuple types must be tightly packed floats");
};

inline constexpr char kVec2Spelling[] = "vec2";
inline constexpr char kVec3Spelling[] = "vec3";
inline constexpr char kVec4Spelling[] = "vec4";
inline constexpr char kQuatSpelling[] = "quat";

template <> struct ColumnTraits<math::Vec2> : FloatTupleTraits<math::Vec2, 2, kVec2Spelling> {};

template <> struct ColumnTraits<math::Vec3> : FloatTupleTraits<math::Vec3, 3, kVec3Spelling> {};

template <> struct ColumnTraits<math::Vec4> : FloatTupleTraits<math::Vec4, 4, kVec4Spelling> {};

template <> struct ColumnTraits<math::Quat> : FloatTupleTraits<math::Quat, 4, kQuatSpelling> {};

template <typename M> void store_elem(typename ColumnTraits<M>::Elem* dst, const M& value) {
    if constexpr (std::is_same_v<M, bool>)
        *dst = value ? 1 : 0;
    else if constexpr (std::is_same_v<M, std::int64_t>)
        *dst = static_cast<double>(value);
    else
        std::memcpy(dst, &value, sizeof(M)); // float and packed float tuples
}

template <typename M> void load_elem(M& dst, const typename ColumnTraits<M>::Elem* src) {
    if constexpr (std::is_same_v<M, bool>)
        dst = *src != 0;
    else if constexpr (std::is_same_v<M, std::int64_t>)
        dst = static_cast<std::int64_t>(*src);
    else {
        // Not memcpy-into-&dst: gcc's -Wclass-memaccess rejects raw byte
        // writes into NSDMI classes (Vec3 et al). bit_cast states the same
        // intent type-safely — M is trivially copyable by construction.
        using Traits = ColumnTraits<M>;
        std::array<typename Traits::Elem, Traits::kWidth> tmp;
        std::memcpy(tmp.data(), src, sizeof(M));
        dst = std::bit_cast<M>(tmp);
    }
}

template <typename T, auto Member>
void gather_column(const ecs::PoolBase& pool,
                   const std::uint32_t* rows,
                   std::uint32_t count,
                   std::byte* dst) {
    using M = std::remove_cvref_t<decltype(std::declval<T>().*Member)>;
    using Traits = ColumnTraits<M>;
    const auto& typed = static_cast<const ecs::Pool<T>&>(pool);
    auto* out = reinterpret_cast<typename Traits::Elem*>(dst);
    for (std::uint32_t i = 0; i < count; ++i)
        store_elem<M>(out + std::size_t{i} * Traits::kWidth, typed.at_dense(rows[i]).*Member);
}

template <typename T, auto Member>
void scatter_column(ecs::PoolBase& pool,
                    const std::uint32_t* rows,
                    std::uint32_t count,
                    const std::byte* src) {
    using M = std::remove_cvref_t<decltype(std::declval<T>().*Member)>;
    using Traits = ColumnTraits<M>;
    auto& typed = static_cast<ecs::Pool<T>&>(pool);
    const auto* in = reinterpret_cast<const typename Traits::Elem*>(src);
    for (std::uint32_t i = 0; i < count; ++i)
        load_elem<M>(typed.at_dense(rows[i]).*Member, in + std::size_t{i} * Traits::kWidth);
}

struct Column {
    std::string field;
    BatchBuffer buffer = BatchBuffer::kF32;
    std::uint32_t width = 1;
    std::size_t elem_bytes = 4; // width * scalar size (bytes per row)
    bool writable = true;       // from the registry: !read_only
    GatherFn gather = nullptr;
    ScatterFn scatter = nullptr;
};

} // namespace batch_detail

// One BatchViews per (ScriptRuntime, World). Lifetime contract: construct,
// expose<T>() every component (boot path), install(), then drive
// refresh/call_tick/commit per phase. Must be destroyed BEFORE the runtime
// and the world; scripts must not call __midday_batch_request afterwards.
class BatchViews {
public:
    BatchViews(ScriptRuntime& runtime, ecs::World& world, const reflect::Registry& registry);
    ~BatchViews();
    BatchViews(const BatchViews&) = delete;
    BatchViews& operator=(const BatchViews&) = delete;
    BatchViews(BatchViews&&) = delete;
    BatchViews& operator=(BatchViews&&) = delete;

    // Boot-path column registration, validated against the ClassDesc the
    // component registered with (abort on mismatch — never a silent drift
    // from the generated bindings_spec).
    template <typename T> class Binder {
    public:
        template <auto Member> Binder& field(std::string_view name) {
            using M = std::remove_cvref_t<decltype(std::declval<T>().*Member)>;
            using Traits = batch_detail::ColumnTraits<M>;
            batch_detail::Column column;
            column.field = std::string(name);
            column.buffer = Traits::kBuffer;
            column.width = Traits::kWidth;
            column.elem_bytes = std::size_t{Traits::kWidth} * sizeof(typename Traits::Elem);
            column.gather = &batch_detail::gather_column<T, Member>;
            column.scatter = &batch_detail::scatter_column<T, Member>;
            owner_->add_column_checked(component_, std::move(column), Traits::kSpelling);
            return *this;
        }

    private:
        friend class BatchViews;

        Binder(BatchViews* owner, std::size_t component) : owner_(owner), component_(component) {}

        BatchViews* owner_;
        std::size_t component_;
    };

    template <typename T> Binder<T> expose(std::string_view component) {
        // Pool identity proof: the by-name pool must BE world.pool<T>() —
        // the typed kernels static_cast on that guarantee.
        const auto* typed = static_cast<const ecs::PoolBase*>(&world().pool<T>());
        return Binder<T>(this, add_component(base::Name(component), typed));
    }

    // Registers the __midday_batch_request hook and the envelope table on
    // the runtime. Call once, after every expose<T>().
    void install();

    // Publish the active join of every request into its typed arrays and
    // stamp envelope.tick. O(pools) boundary crossings; O(entities) C++.
    std::optional<base::Error> refresh(std::uint64_t tick);

    // Invoke globalThis.__midday_batch_tick(tick) (set via midday/batch
    // onTick). bindings.no_tick_entry when the script registered none.
    std::optional<base::Error> call_tick(std::uint64_t tick);

    // Scatter writable columns back into dense storage — THE deterministic
    // write-back point. bindings.stale_view if a gathered row vanished.
    std::optional<base::Error> commit();

    [[nodiscard]] const BatchStats& stats() const;

    [[nodiscard]] std::size_t exposed_components() const;

private:
    [[nodiscard]] ecs::World& world();
    std::size_t add_component(base::Name name, const ecs::PoolBase* expected_pool);
    void add_column_checked(std::size_t component,
                            batch_detail::Column column,
                            std::string_view spelling);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace midday::script
