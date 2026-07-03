// ts/runtime/batch_views.cpp — staged SoA batch views over ECS pools (see
// batch_views.h for the decision record). Speaks QuickJS through the
// script_runtime.h detail seam: typed arrays cannot cross the JSON hook
// seam, and this file is the ONLY consumer of that exception.

#include "ts/runtime/batch_views.h"

#include "core/ecs/view.h" // ecs::detail::scan_active_rows — the one word-scan

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <quickjs.h>
#include <span>

namespace midday::script {
namespace {

using base::Error;
using base::Json;

// Boot-path misuse (unknown component/field, spec drift) aborts loudly —
// the reflect registration discipline (D-BUILD-023).
[[noreturn]] void fatal_bind(const std::string& message) {
    std::fprintf(stderr, "midday: fatal: bindings: %s\n", message.c_str());
    std::abort();
}

JSTypedArrayEnum typed_array_kind(BatchBuffer buffer) {
    switch (buffer) {
    case BatchBuffer::kF32:
        return JS_TYPED_ARRAY_FLOAT32;
    case BatchBuffer::kF64:
        return JS_TYPED_ARRAY_FLOAT64;
    case BatchBuffer::kU8:
        break;
    }
    return JS_TYPED_ARRAY_UINT8;
}

struct Component {
    base::Name name;
    ecs::PoolBase* pool = nullptr;
    std::vector<batch_detail::Column> columns;
};

// One component's segment of a request: requested columns, the JS view
// object, and the C++-owned staging memory its ArrayBuffers alias.
struct Slot {
    std::size_t component = 0;
    std::vector<std::uint32_t> columns; // indices into Component::columns
    std::vector<std::uint32_t> rows;    // dense rows aligned with entities
    std::uint32_t capacity = 0;
    JSValue view = JS_UNDEFINED;
    JSValue buffers = JS_UNDEFINED;
    std::vector<JSValue> array_buffers;
    std::vector<JSValue> typed_arrays;
    std::vector<std::vector<double>> staging; // double-aligned backing
};

struct Request {
    std::vector<Slot> slots;
    std::vector<std::uint32_t> entities; // the active join, refresh order
    JSValue envelope = JS_UNDEFINED;
};

Error bad_request(std::string message) {
    return Error{.code = "bindings.bad_request", .message = std::move(message)};
}

} // namespace

struct BatchViews::Impl {
    ScriptRuntime* runtime = nullptr;
    ecs::World* world = nullptr;
    const reflect::Registry* registry = nullptr;
    JSContext* ctx = nullptr;
    std::vector<Component> components;
    std::vector<Request> requests;
    JSValue envelopes = JS_UNDEFINED; // globalThis.__midday_batch_envelopes
    std::vector<std::uint32_t> join_scratch;
    BatchStats stats;
    bool installed = false;

    [[nodiscard]] const Component* find_component(std::string_view name) const {
        for (const Component& component : components)
            if (component.name.view() == name)
                return &component;
        return nullptr;
    }

    // ---- request construction (inside the __midday_batch_request hook) ---
    HostResult build_request(const Json::Array& args);
    JSValue build_envelope(Request& request);

    // ---- the per-phase cycle ---------------------------------------------
    void join(Request& request);
    std::optional<Error> grow(Slot& slot, std::uint32_t needed);
    std::optional<Error> refresh(std::uint64_t tick);
    std::optional<Error> commit();

    void release_js_state() {
        for (Request& request : requests) {
            for (Slot& slot : request.slots) {
                for (std::size_t i = 0; i < slot.array_buffers.size(); ++i) {
                    if (JS_IsUndefined(slot.array_buffers[i]))
                        continue;
                    JS_DetachArrayBuffer(ctx, slot.array_buffers[i]);
                    JS_FreeValue(ctx, slot.array_buffers[i]);
                    JS_FreeValue(ctx, slot.typed_arrays[i]);
                }
                JS_FreeValue(ctx, slot.buffers);
                JS_FreeValue(ctx, slot.view);
            }
            JS_FreeValue(ctx, request.envelope);
        }
        requests.clear();
        JS_FreeValue(ctx, envelopes);
        envelopes = JS_UNDEFINED;
    }
};

HostResult BatchViews::Impl::build_request(const Json::Array& args) {
    HostResult out;
    if (args.size() != 1 || !args[0].is_object()) {
        out.error = bad_request("__midday_batch_request expects one {components:[...]} object");
        return out;
    }
    const Json* list = args[0].find("components");
    if (list == nullptr || !list->is_array() || list->elements().empty()) {
        out.error = bad_request("batch request needs a non-empty components array");
        return out;
    }
    Request request;
    for (const Json& entry : list->elements()) {
        const Json* name = entry.is_object() ? entry.find("component") : nullptr;
        const Json* fields = entry.is_object() ? entry.find("fields") : nullptr;
        if (name == nullptr || !name->is_string() || fields == nullptr || !fields->is_array() ||
            fields->elements().empty()) {
            out.error = bad_request("each request entry is {component, fields:[...]} (non-empty)");
            return out;
        }
        const Component* component = find_component(name->as_string());
        if (component == nullptr) {
            out.error =
                bad_request("component '" + name->as_string() + "' is not exposed to batch views");
            out.error->details.set("component", name->as_string());
            return out;
        }
        Slot slot;
        slot.component = static_cast<std::size_t>(component - components.data());
        for (const Json& field : fields->elements()) {
            const auto matches = [&](std::uint32_t index) {
                return component->columns[index].field == field.as_string();
            };
            std::uint32_t column = 0;
            while (column < component->columns.size() && !(field.is_string() && matches(column)))
                ++column;
            const bool duplicate = std::ranges::find(slot.columns, column) != slot.columns.end();
            if (column == component->columns.size() || duplicate) {
                out.error = bad_request("unknown or duplicate field in batch request for '" +
                                        name->as_string() + "'");
                out.error->details.set("component", name->as_string());
                out.error->details.set("field", field.is_string() ? field.as_string() : "");
                return out;
            }
            slot.columns.push_back(column);
        }
        slot.array_buffers.assign(slot.columns.size(), JS_UNDEFINED);
        slot.typed_arrays.assign(slot.columns.size(), JS_UNDEFINED);
        slot.staging.resize(slot.columns.size());
        request.slots.push_back(std::move(slot));
    }

    const auto id = static_cast<std::uint32_t>(requests.size());
    const JSValue envelope = build_envelope(request);
    requests.push_back(std::move(request));
    JS_SetPropertyUint32(ctx, envelopes, id, envelope); // consumes envelope
    out.value = Json::object();
    out.value.set("request", static_cast<std::int64_t>(id));
    out.value.set("envelope_version", kBatchEnvelopeVersion);
    return out;
}

JSValue BatchViews::Impl::build_envelope(Request& request) {
    const JSValue envelope = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, envelope, "envelope_version", JS_NewInt64(ctx, kBatchEnvelopeVersion));
    JS_SetPropertyStr(ctx, envelope, "tick", JS_NewInt32(ctx, 0));
    const JSValue views = JS_NewArray(ctx);
    for (std::size_t i = 0; i < request.slots.size(); ++i) {
        Slot& slot = request.slots[i];
        const Component& component = components[slot.component];
        const JSValue view = JS_NewObject(ctx);
        const std::string_view name = component.name.view();
        JS_SetPropertyStr(ctx, view, "component", JS_NewStringLen(ctx, name.data(), name.size()));
        const JSValue fields = JS_NewArray(ctx);
        for (std::size_t j = 0; j < slot.columns.size(); ++j) {
            const std::string& field = component.columns[slot.columns[j]].field;
            JS_SetPropertyUint32(ctx,
                                 fields,
                                 static_cast<std::uint32_t>(j),
                                 JS_NewStringLen(ctx, field.data(), field.size()));
        }
        JS_SetPropertyStr(ctx, view, "fields", fields);
        JS_SetPropertyStr(ctx, view, "count", JS_NewInt32(ctx, 0));
        slot.buffers = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, view, "buffers", JS_DupValue(ctx, slot.buffers));
        slot.view = JS_DupValue(ctx, view);
        JS_SetPropertyUint32(ctx, views, static_cast<std::uint32_t>(i), view);
    }
    JS_SetPropertyStr(ctx, envelope, "views", views);
    request.envelope = JS_DupValue(ctx, envelope);
    return envelope;
}

// The active join: driver = smallest pool (first-smallest tie-break, the
// view.h rule), word-scanned; other slots pay one paged find + active test
// per candidate. Deterministic: driver dense order.
void BatchViews::Impl::join(Request& request) {
    request.entities.clear();
    for (Slot& slot : request.slots)
        slot.rows.clear();
    std::size_t driver = 0;
    for (std::size_t i = 1; i < request.slots.size(); ++i) {
        const auto size_of = [&](std::size_t slot) {
            return components[request.slots[slot].component].pool->set().size();
        };
        if (size_of(i) < size_of(driver))
            driver = i;
    }
    const ecs::SparseSet& driver_set = components[request.slots[driver].component].pool->set();
    join_scratch.resize(request.slots.size());
    ecs::detail::scan_active_rows(
        driver_set.active_words(), driver_set.size(), false, [&](std::uint32_t dpos) {
            const std::uint32_t entity = driver_set.dense()[dpos];
            for (std::size_t i = 0; i < request.slots.size(); ++i) {
                if (i == driver)
                    continue;
                const ecs::SparseSet& set = components[request.slots[i].component].pool->set();
                const std::uint32_t pos = set.find(entity);
                if (pos == ecs::kNpos || !set.is_active(pos))
                    return; // this candidate misses; scan continues
                join_scratch[i] = pos;
            }
            join_scratch[driver] = dpos; // all slots matched — commit the row
            for (std::size_t i = 0; i < request.slots.size(); ++i)
                request.slots[i].rows.push_back(join_scratch[i]);
            request.entities.push_back(entity);
        });
}

std::optional<Error> BatchViews::Impl::grow(Slot& slot, std::uint32_t needed) {
    const Component& component = components[slot.component];
    const std::uint32_t capacity = std::bit_ceil(std::max(needed, 8u));
    for (std::size_t j = 0; j < slot.columns.size(); ++j) {
        const batch_detail::Column& column = component.columns[slot.columns[j]];
        if (!JS_IsUndefined(slot.array_buffers[j])) {
            // Deterministic invalidation: stale JS references see a
            // detached buffer (length 0), never freed memory.
            JS_DetachArrayBuffer(ctx, slot.array_buffers[j]);
            JS_FreeValue(ctx, slot.array_buffers[j]);
            JS_FreeValue(ctx, slot.typed_arrays[j]);
            slot.array_buffers[j] = JS_UNDEFINED;
            slot.typed_arrays[j] = JS_UNDEFINED;
        }
        const std::size_t bytes = std::size_t{capacity} * column.elem_bytes;
        slot.staging[j].assign((bytes + sizeof(double) - 1) / sizeof(double), 0.0);
        JSValue buffer = JS_NewArrayBuffer(ctx,
                                           reinterpret_cast<std::uint8_t*>(slot.staging[j].data()),
                                           bytes,
                                           nullptr,
                                           nullptr,
                                           false);
        if (JS_IsException(buffer))
            return detail::runtime_take_exception(*runtime);
        // Explicit (buffer, byteOffset, length): the constructor peeks at
        // argv[1]/argv[2] positionally, so all three are always passed.
        const std::uint32_t elems = capacity * column.width;
        JSValue argv[] = {
            buffer, JS_NewInt32(ctx, 0), JS_NewInt32(ctx, static_cast<std::int32_t>(elems))};
        const JSValue array = JS_NewTypedArray(ctx, 3, argv, typed_array_kind(column.buffer));
        if (JS_IsException(array)) {
            JS_FreeValue(ctx, buffer);
            return detail::runtime_take_exception(*runtime);
        }
        slot.array_buffers[j] = buffer; // keep our ref for the next detach
        slot.typed_arrays[j] = JS_DupValue(ctx, array);
        JS_SetPropertyStr(ctx, slot.buffers, column.field.c_str(), array);
    }
    slot.capacity = capacity;
    ++stats.view_rebuilds;
    return std::nullopt;
}

std::optional<Error> BatchViews::Impl::refresh(std::uint64_t tick) {
    for (Request& request : requests) {
        join(request);
        const auto count = static_cast<std::uint32_t>(request.entities.size());
        for (Slot& slot : request.slots) {
            if (count > slot.capacity)
                if (auto error = grow(slot, count))
                    return error;
            const Component& component = components[slot.component];
            for (std::size_t j = 0; j < slot.columns.size(); ++j) {
                const batch_detail::Column& column = component.columns[slot.columns[j]];
                column.gather(*component.pool,
                              slot.rows.data(),
                              count,
                              reinterpret_cast<std::byte*>(slot.staging[j].data()));
                ++stats.buffer_refreshes;
            }
            JS_SetPropertyStr(
                ctx, slot.view, "count", JS_NewInt32(ctx, static_cast<std::int32_t>(count)));
        }
        JS_SetPropertyStr(
            ctx, request.envelope, "tick", JS_NewFloat64(ctx, static_cast<double>(tick)));
    }
    return std::nullopt;
}

std::optional<Error> BatchViews::Impl::commit() {
    for (std::size_t r = 0; r < requests.size(); ++r) {
        Request& request = requests[r];
        const auto count = static_cast<std::uint32_t>(request.entities.size());
        for (Slot& slot : request.slots) {
            Component& component = components[slot.component];
            const bool any_writable = std::ranges::any_of(slot.columns, [&](std::uint32_t index) {
                return component.columns[index].writable;
            });
            if (!any_writable)
                continue;
            // Re-find every gathered entity: swap-and-pop moves are
            // followed; a vanished row refuses the commit deterministically.
            const ecs::SparseSet& set = component.pool->set();
            for (std::uint32_t i = 0; i < count; ++i) {
                const std::uint32_t pos = set.find(request.entities[i]);
                if (pos == ecs::kNpos) {
                    Error error{.code = "bindings.stale_view",
                                .message = "batch commit refused: entity row vanished between "
                                           "refresh and commit (structural change mid-phase)"};
                    error.details.set("component", component.name.view());
                    error.details.set("entity_index",
                                      static_cast<std::int64_t>(request.entities[i]));
                    error.details.set("request", static_cast<std::int64_t>(r));
                    return error;
                }
                slot.rows[i] = pos;
            }
            for (std::size_t j = 0; j < slot.columns.size(); ++j) {
                const batch_detail::Column& column = component.columns[slot.columns[j]];
                if (!column.writable)
                    continue;
                column.scatter(*component.pool,
                               slot.rows.data(),
                               count,
                               reinterpret_cast<const std::byte*>(slot.staging[j].data()));
                ++stats.buffer_commits;
            }
        }
    }
    return std::nullopt;
}

BatchViews::BatchViews(ScriptRuntime& runtime, ecs::World& world, const reflect::Registry& registry)
    : impl_(std::make_unique<Impl>()) {
    impl_->runtime = &runtime;
    impl_->world = &world;
    impl_->registry = &registry;
    impl_->ctx = detail::runtime_context(runtime);
}

BatchViews::~BatchViews() {
    if (impl_->installed) {
        // Last registration wins: scripts calling the hook after teardown
        // get a structured refusal, never a dangling capture.
        impl_->runtime->register_host_fn("__midday_batch_request", [](const Json::Array&) {
            HostResult out;
            out.error =
                Error{.code = "bindings.detached", .message = "the batch view host was torn down"};
            return out;
        });
    }
    impl_->release_js_state();
}

ecs::World& BatchViews::world() {
    return *impl_->world;
}

std::size_t BatchViews::add_component(base::Name name, const ecs::PoolBase* expected_pool) {
    if (impl_->installed)
        fatal_bind("expose() after install() — column registration is boot-path only");
    if (impl_->find_component(name.view()) != nullptr)
        fatal_bind("component '" + std::string(name.view()) + "' exposed twice");
    if (impl_->registry->find_class(name) == nullptr)
        fatal_bind("component '" + std::string(name.view()) +
                   "' has no registered ClassDesc — batch columns mirror the reflected API");
    ecs::PoolBase* pool = nullptr;
    for (ecs::PoolBase* candidate : impl_->world->pools_in_registration_order())
        if (candidate->name() == name)
            pool = candidate;
    if (pool == nullptr || pool != expected_pool)
        fatal_bind("component '" + std::string(name.view()) +
                   "' does not match its registered pool (wrong T in expose<T>?)");
    impl_->components.push_back(Component{.name = name, .pool = pool, .columns = {}});
    return impl_->components.size() - 1;
}

void BatchViews::add_column_checked(std::size_t component,
                                    batch_detail::Column column,
                                    std::string_view spelling) {
    Component& entry = impl_->components[component];
    for (const batch_detail::Column& existing : entry.columns)
        if (existing.field == column.field)
            fatal_bind("field '" + column.field + "' bound twice on '" +
                       std::string(entry.name.view()) + "'");
    const reflect::Registered<reflect::ClassDesc>* cls = impl_->registry->find_class(entry.name);
    const reflect::PropertyDesc* property = nullptr;
    for (const reflect::PropertyDesc& candidate : cls->desc.properties)
        if (candidate.name.view() == column.field)
            property = &candidate;
    if (property == nullptr)
        fatal_bind("field '" + column.field + "' is not a property of '" +
                   std::string(entry.name.view()) + "'");
    if (property->type.canonical() != spelling)
        fatal_bind("field '" + column.field + "' on '" + std::string(entry.name.view()) + "' is " +
                   property->type.canonical() + ", bound as " + std::string(spelling));
    column.writable = (property->flags & reflect::kPropertyReadOnly) == 0;
    entry.columns.push_back(std::move(column));
}

void BatchViews::install() {
    if (impl_->installed)
        fatal_bind("install() called twice");
    impl_->installed = true;
    JSContext* ctx = impl_->ctx;
    impl_->envelopes = JS_NewArray(ctx);
    const JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "__midday_batch_envelopes", JS_DupValue(ctx, impl_->envelopes));
    JS_FreeValue(ctx, global);
    Impl* impl = impl_.get();
    impl_->runtime->register_host_fn("__midday_batch_request", [impl](const Json::Array& args) {
        return impl->build_request(args);
    });
}

std::optional<Error> BatchViews::refresh(std::uint64_t tick) {
    return impl_->refresh(tick);
}

std::optional<Error> BatchViews::call_tick(std::uint64_t tick) {
    JSContext* ctx = impl_->ctx;
    const JSValue global = JS_GetGlobalObject(ctx);
    const JSValue callee = JS_GetPropertyStr(ctx, global, "__midday_batch_tick");
    JS_FreeValue(ctx, global);
    if (!JS_IsFunction(ctx, callee)) {
        JS_FreeValue(ctx, callee);
        return Error{.code = "bindings.no_tick_entry",
                     .message = "no __midday_batch_tick registered — scripts install one via "
                                "midday/batch onTick()"};
    }
    JSValue arg = JS_NewFloat64(ctx, static_cast<double>(tick));
    const JSValue result = JS_Call(ctx, callee, JS_UNDEFINED, 1, &arg);
    JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, callee);
    ++impl_->stats.tick_calls;
    if (JS_IsException(result))
        return detail::runtime_take_exception(*impl_->runtime);
    JS_FreeValue(ctx, result);
    return std::nullopt;
}

std::optional<Error> BatchViews::commit() {
    return impl_->commit();
}

const BatchStats& BatchViews::stats() const {
    return impl_->stats;
}

std::size_t BatchViews::exposed_components() const {
    return impl_->components.size();
}

} // namespace midday::script
