// bindings.* doctests — the batch view runtime: SoA typed-array publishing,
// active-join semantics, deterministic write-back, read_only enforcement,
// stale-view refusal, detach-on-growth invalidation, and the two budget
// claims (crossings scale with buffers not entities; steady-state ticks
// allocate zero GC bytes). Scripts here are raw JS via eval_global — the
// TS-fixture path is covered by the bindings.bench cases.

#include "core/ecs/world.h"
#include "core/math/vec.h"
#include "core/reflect/registry.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"
#include "ts/runtime/batch_views.h"
#include "ts/runtime/script_runtime.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace base = midday::base;
namespace ecs = midday::ecs;
namespace math = midday::math;
namespace reflect = midday::reflect;
namespace script = midday::script;

using base::Error;
using base::Json;
using midday::testkit::unwrap;

struct TestPoint {
    float x, y;
};

struct TestBody {
    math::Vec3 pos;
    bool alive;
    std::int64_t score;
};

struct TestHealth {
    float current;
    float max;
};

reflect::ClassDesc
test_class(std::string_view name,
           std::initializer_list<std::pair<std::string_view, reflect::TypeKind>> fields,
           std::string_view read_only_field = "") {
    reflect::ClassDesc desc;
    desc.name = midday::base::Name(name);
    for (const auto& [field, kind] : fields)
        desc.properties.push_back(reflect::PropertyDesc{
            .name = midday::base::Name(field),
            .type = reflect::TypeDesc::scalar(kind),
            .default_value = Json(),
            .flags = field == read_only_field ? reflect::kPropertyReadOnly : 0U,
            .doc = ""});
    return desc;
}

// One world with the three fixture components; entities spawn with point +
// health always, body optionally.
struct Fixture {
    reflect::Registry registry;
    ecs::World world{registry};
    script::ScriptRuntime runtime;
    script::BatchViews views{runtime, world, registry};
    std::vector<ecs::EntityRef> refs;

    Fixture() {
        using reflect::TypeKind;
        world.register_component<TestPoint>(
            test_class("point", {{"x", TypeKind::kFloat}, {"y", TypeKind::kFloat}}));
        world.register_component<TestBody>(test_class(
            "body",
            {{"pos", TypeKind::kVec3}, {"alive", TypeKind::kBool}, {"score", TypeKind::kInt}}));
        world.register_component<TestHealth>(test_class(
            "health", {{"current", TypeKind::kFloat}, {"max", TypeKind::kFloat}}, "max"));
        views.expose<TestPoint>("point").field<&TestPoint::x>("x").field<&TestPoint::y>("y");
        views.expose<TestBody>("body")
            .field<&TestBody::pos>("pos")
            .field<&TestBody::alive>("alive")
            .field<&TestBody::score>("score");
        views.expose<TestHealth>("health")
            .field<&TestHealth::current>("current")
            .field<&TestHealth::max>("max");
        views.install();
    }

    ecs::EntityRef spawn(float x, float y, float hp, bool with_body = false) {
        const ecs::EntityRef ref = world.spawn();
        world.emplace(ref, TestPoint{x, y});
        world.emplace(ref, TestHealth{hp, 100.0F});
        if (with_body)
            world.emplace(ref, TestBody{{x, y, x + y}, true, static_cast<std::int64_t>(x)});
        refs.push_back(ref);
        return ref;
    }

    void eval(const std::string& js) {
        std::optional<Error> error = runtime.eval_global(js, "bindings_test.js");
        REQUIRE_MESSAGE(!error.has_value(), (error ? error->message : std::string()));
    }

    // Evaluate a zero-arg function expression and JSON-return its result.
    Json call(const std::string& fn_expr) {
        eval("globalThis.__probe = " + fn_expr + ";");
        script::EvalResult result = runtime.call_json("__probe", Json::object());
        REQUIRE_MESSAGE(!result.error.has_value(),
                        (result.error ? result.error->message : std::string()));
        return std::move(result.value);
    }
};

void expect_ok(std::optional<Error> error) {
    REQUIRE_MESSAGE(!error.has_value(), (error ? error->code + ": " + error->message : ""));
}

constexpr const char* kRequestPointHealth = R"js(
const granted = __midday_batch_request({components: [
    {component: "point", fields: ["x", "y"]},
    {component: "health", fields: ["current", "max"]}]});
globalThis.env = __midday_batch_envelopes[granted.request];
globalThis.version = granted.envelope_version;
)js";

} // namespace

TEST_CASE("bindings.views: request publishes an aligned active join; commit scatters writes") {
    Fixture fx;
    fx.spawn(1.0F, 10.0F, 50.0F);
    fx.spawn(2.0F, 20.0F, 60.0F);
    fx.spawn(3.0F, 30.0F, 70.0F);
    fx.eval(kRequestPointHealth);
    CHECK(fx.call("(() => version)").as_int() == script::kBatchEnvelopeVersion);

    expect_ok(fx.views.refresh(7));
    const Json snapshot = fx.call(R"js((() => ({
        tick: env.tick,
        count: env.views[0].count,
        aligned: env.views[1].count === env.views[0].count,
        x: Array.from(env.views[0].buffers.x.slice(0, env.views[0].count)),
        hp: Array.from(env.views[1].buffers.current.slice(0, env.views[1].count)),
        f32: env.views[0].buffers.x instanceof Float32Array,
        fields: env.views[0].fields.join(","),
    })))js");
    CHECK(snapshot.find("tick")->as_int() == 7);
    CHECK(snapshot.find("count")->as_int() == 3);
    CHECK(snapshot.find("aligned")->as_bool());
    CHECK(snapshot.find("f32")->as_bool());
    CHECK(snapshot.find("fields")->as_string() == "x,y");
    CHECK(snapshot.find("x")->elements()[1].as_double() == 2.0);
    CHECK(snapshot.find("hp")->elements()[2].as_double() == 70.0);

    // Script writes; nothing lands until commit (staged write-back point).
    fx.eval("env.views[0].buffers.x[0] = 42; env.views[1].buffers.current[2] = 5;");
    CHECK(fx.world.try_get<TestPoint>(fx.refs[0])->x == 1.0F);
    expect_ok(fx.views.commit());
    CHECK(fx.world.try_get<TestPoint>(fx.refs[0])->x == 42.0F);
    CHECK(fx.world.try_get<TestHealth>(fx.refs[2])->current == 5.0F);
}

TEST_CASE("bindings.views: inactive rows and missing components leave the join") {
    Fixture fx;
    fx.spawn(1.0F, 0.0F, 1.0F);
    const ecs::EntityRef toggled = fx.spawn(2.0F, 0.0F, 2.0F);
    fx.spawn(3.0F, 0.0F, 3.0F);
    const ecs::EntityRef point_only = fx.world.spawn(); // no health: never joins
    fx.world.emplace(point_only, TestPoint{9.0F, 9.0F});
    fx.world.set_active<TestHealth>(toggled, false);

    fx.eval(kRequestPointHealth);
    expect_ok(fx.views.refresh(0));
    const Json xs =
        fx.call("(() => Array.from(env.views[0].buffers.x.slice(0, env.views[0].count)))");
    REQUIRE(xs.elements().size() == 2); // toggled + point_only excluded
    CHECK(xs.elements()[0].as_double() == 1.0);
    CHECK(xs.elements()[1].as_double() == 3.0);
}

TEST_CASE("bindings.views: read_only columns publish but never scatter back") {
    Fixture fx;
    fx.spawn(1.0F, 0.0F, 50.0F);
    fx.eval(kRequestPointHealth);
    expect_ok(fx.views.refresh(0));
    fx.eval("env.views[1].buffers.max[0] = 1; env.views[1].buffers.current[0] = 2;");
    expect_ok(fx.views.commit());
    CHECK(fx.world.try_get<TestHealth>(fx.refs[0])->max == 100.0F); // untouched
    CHECK(fx.world.try_get<TestHealth>(fx.refs[0])->current == 2.0F);
}

TEST_CASE("bindings.views: vec3/bool/int columns cross as f32x3, u8, f64") {
    Fixture fx;
    fx.spawn(4.0F, 5.0F, 1.0F, true);
    fx.eval(R"js(
const granted = __midday_batch_request({components: [
    {component: "body", fields: ["pos", "alive", "score"]}]});
globalThis.env = __midday_batch_envelopes[granted.request];
)js");
    expect_ok(fx.views.refresh(0));
    const Json shape = fx.call(R"js((() => ({
        pos: Array.from(env.views[0].buffers.pos.slice(0, 3)),
        pos_f32: env.views[0].buffers.pos instanceof Float32Array,
        alive_u8: env.views[0].buffers.alive instanceof Uint8Array,
        score_f64: env.views[0].buffers.score instanceof Float64Array,
        alive: env.views[0].buffers.alive[0],
        score: env.views[0].buffers.score[0],
    })))js");
    CHECK(shape.find("pos_f32")->as_bool());
    CHECK(shape.find("alive_u8")->as_bool());
    CHECK(shape.find("score_f64")->as_bool());
    CHECK(shape.find("pos")->elements()[2].as_double() == 9.0);
    CHECK(shape.find("alive")->as_int() == 1);
    CHECK(shape.find("score")->as_int() == 4);

    fx.eval("env.views[0].buffers.pos[1] = 77; env.views[0].buffers.alive[0] = 0;"
            "env.views[0].buffers.score[0] = 123;");
    expect_ok(fx.views.commit());
    const TestBody* body = fx.world.try_get<TestBody>(fx.refs[0]);
    CHECK(body->pos.y == 77.0F);
    CHECK_FALSE(body->alive);
    CHECK(body->score == 123);
}

TEST_CASE("bindings.views: growth detaches stale buffers deterministically") {
    Fixture fx;
    fx.spawn(1.0F, 0.0F, 1.0F);
    fx.eval(kRequestPointHealth);
    expect_ok(fx.views.refresh(0));
    fx.eval("globalThis.stale = env.views[0].buffers.x;");
    for (int i = 0; i < 32; ++i) // beyond the initial capacity
        fx.spawn(static_cast<float>(i), 0.0F, 1.0F);
    expect_ok(fx.views.refresh(1));
    const Json probe = fx.call(R"js((() => ({
        stale_len: stale.length,
        stale_read: typeof stale[0],
        fresh_len_ok: env.views[0].buffers.x.length >= env.views[0].count,
        fresh: env.views[0].buffers.x[4],
    })))js");
    CHECK(probe.find("stale_len")->as_int() == 0); // detached: dead, not dangling
    CHECK(probe.find("stale_read")->as_string() == "undefined");
    CHECK(probe.find("fresh_len_ok")->as_bool());
}

TEST_CASE("bindings.views: rows that vanish between refresh and commit refuse stale_view") {
    Fixture fx;
    fx.spawn(1.0F, 0.0F, 1.0F);
    fx.spawn(2.0F, 0.0F, 2.0F);
    fx.eval(kRequestPointHealth);
    expect_ok(fx.views.refresh(0));
    CHECK_FALSE(fx.world.despawn(fx.refs[0]).has_value());
    std::optional<Error> refused = fx.views.commit();
    const Error& error = unwrap(refused);
    CHECK(error.code == "bindings.stale_view");
    CHECK(error.details.find("component") != nullptr);
}

TEST_CASE("bindings.views: commit follows swap-and-pop moves of surviving rows") {
    Fixture fx;
    fx.spawn(1.0F, 0.0F, 1.0F);
    fx.spawn(2.0F, 0.0F, 2.0F);
    const ecs::EntityRef bystander = fx.world.spawn(); // point-only: not in the join
    fx.world.emplace(bystander, TestPoint{9.0F, 9.0F});
    fx.eval(kRequestPointHealth);
    expect_ok(fx.views.refresh(0));
    fx.eval("env.views[0].buffers.x[0] = 111; env.views[0].buffers.x[1] = 222;");
    // Dropping the bystander swap-and-pops the point pool mid-phase; commit
    // re-finds by entity, so the writes land on the right survivors.
    CHECK_FALSE(fx.world.despawn(bystander).has_value());
    expect_ok(fx.views.commit());
    CHECK(fx.world.try_get<TestPoint>(fx.refs[0])->x == 111.0F);
    CHECK(fx.world.try_get<TestPoint>(fx.refs[1])->x == 222.0F);
}

TEST_CASE("bindings.views: malformed requests throw structured bindings.bad_request") {
    Fixture fx;
    fx.spawn(1.0F, 0.0F, 1.0F);
    fx.eval(R"js(
globalThis.messages = [];
for (const bad of [
    {components: []},
    {components: [{component: "nope", fields: ["x"]}]},
    {components: [{component: "point", fields: ["nope"]}]},
    {components: [{component: "point", fields: ["x", "x"]}]},
]) {
    try { __midday_batch_request(bad); messages.push("NO THROW"); }
    catch (e) { messages.push(String(e.message)); }
}
)js");
    const Json messages = fx.call("(() => messages)");
    REQUIRE(messages.elements().size() == 4);
    for (const Json& message : messages.elements())
        CHECK(message.as_string().find("bindings.bad_request") != std::string::npos);
}

TEST_CASE("bindings.views: no registered tick entry is a structured refusal") {
    Fixture fx;
    std::optional<Error> refused = fx.views.call_tick(0);
    CHECK(unwrap(refused).code == "bindings.no_tick_entry");
}

TEST_CASE("bindings.views: crossings scale with buffer count, never entity count") {
    const auto refreshes_for = [](std::uint32_t entities) {
        Fixture fx;
        for (std::uint32_t i = 0; i < entities; ++i)
            fx.spawn(static_cast<float>(i), 0.0F, 1.0F);
        fx.eval(kRequestPointHealth);
        expect_ok(fx.views.refresh(0));
        expect_ok(fx.views.commit());
        const script::BatchStats& stats = fx.views.stats();
        return std::pair{stats.buffer_refreshes, stats.buffer_commits};
    };
    const auto small = refreshes_for(16);
    const auto large = refreshes_for(512);
    CHECK(small.first == 4);  // x, y, current, max
    CHECK(small.second == 3); // max is read_only
    CHECK(small == large);    // O(buffers), not O(entities)
}

TEST_CASE("bindings.views: steady-state ticks allocate zero GC bytes") {
    Fixture fx;
    for (int i = 0; i < 64; ++i)
        fx.spawn(static_cast<float>(i), 1.0F, 50.0F);
    fx.eval(kRequestPointHealth);
    // A pooled-math-shaped tick: reused closure, reused pooled object,
    // typed-array arithmetic — nothing allocated per invocation.
    fx.eval(R"js(
const scratch = {x: 0, y: 0, z: 0};
globalThis.__midday_batch_tick = (tick) => {
    const view = env.views[0];
    const xs = view.buffers.x, ys = view.buffers.y;
    scratch.x = tick * 0.25; scratch.y = 1.0; scratch.z = scratch.x + scratch.y;
    const n = view.count;
    for (let i = 0; i < n; i++) { xs[i] += scratch.x * 0.01; ys[i] += xs[i] * 0.5; }
};
)js");
    for (std::uint64_t tick = 0; tick < 5; ++tick) { // warmup: shapes, growth
        expect_ok(fx.views.refresh(tick));
        expect_ok(fx.views.call_tick(tick));
        expect_ok(fx.views.commit());
    }
    const std::uint64_t before = fx.runtime.alloc_bytes();
    for (std::uint64_t tick = 5; tick < 25; ++tick) {
        expect_ok(fx.views.refresh(tick));
        expect_ok(fx.views.call_tick(tick));
        expect_ok(fx.views.commit());
    }
    CHECK(fx.runtime.alloc_bytes() - before == 0);
}
