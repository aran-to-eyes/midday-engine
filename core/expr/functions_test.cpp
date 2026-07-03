// expr.fn.* — the function inventory contract: every builtin registers
// through core/reflect as a MethodDesc free function (D-BUILD-021 — method
// signatures at registry scope, no parallel model), with param/return types
// and per-function compat hashes. This is exactly what `midday api dump`
// (m0-api-json) walks — the API-dump exit test asserts here at the registry
// level, before the CLI verb exists.

#include "core/base/hex.h"
#include "core/base/name.h"
#include "core/expr/functions.h"
#include "core/reflect/registry.h"
#include "testkit/doctest.h"
#include "testkit/doctest_unwrap.h"

#include <cstddef>
#include <ostream> // MSVC: doctest stringifies string_view via operator<<
#include <set>
#include <string>
#include <vector>

using namespace midday;

TEST_CASE("expr.fn: inventory registers 1:1 through the reflect registry") {
    reflect::Registry registry;
    expr::register_expr_functions(registry);

    const auto entries = registry.functions();
    const auto inventory = expr::builtins();
    REQUIRE(entries.size() == inventory.size());

    // Registration order == inventory order == enumeration order (CORE
    // level, deterministic enumeration contract).
    for (std::size_t i = 0; i < inventory.size(); ++i) {
        const expr::Builtin& builtin = inventory[i];
        const reflect::MethodDesc& desc = entries[i]->desc;
        CHECK(desc.name.view() == builtin.name);
        REQUIRE(desc.params.size() == builtin.arity);
        for (std::size_t p = 0; p < desc.params.size(); ++p) {
            CHECK(desc.params[p].name.view() == builtin.param_names[p]);
            CHECK(desc.params[p].type.canonical() ==
                  expr::to_type_desc(builtin.params[p]).canonical());
            CHECK(desc.params[p].default_value.is_null()); // all params required
        }
        CHECK(testkit::unwrap(desc.returns).canonical() ==
              expr::to_type_desc(builtin.returns).canonical());
        CHECK(!desc.doc.empty());
        CHECK(desc.compat_hash != 0); // per-method compat hash (spec 4.3)
    }
}

TEST_CASE("expr.fn: the expected functions are present with exact signatures") {
    reflect::Registry registry;
    expr::register_expr_functions(registry);

    const auto expect = [&](std::string_view name, std::string_view signature) {
        const auto* entry = registry.find_function(base::Name(name));
        REQUIRE_MESSAGE(entry != nullptr, name);
        std::string spelled = std::string(entry->desc.name.view()) + "(";
        for (std::size_t i = 0; i < entry->desc.params.size(); ++i) {
            if (i > 0)
                spelled += ", ";
            spelled += entry->desc.params[i].type.canonical();
        }
        spelled += ") -> " + testkit::unwrap(entry->desc.returns).canonical();
        CHECK(spelled == signature);
    };

    expect("int", "int(float) -> int");
    expect("float", "float(int) -> float");
    expect("abs", "abs(float) -> float");
    expect("sign", "sign(float) -> float");
    expect("floor", "floor(float) -> float");
    expect("ceil", "ceil(float) -> float");
    expect("round", "round(float) -> float");
    expect("trunc", "trunc(float) -> float");
    expect("fract", "fract(float) -> float");
    expect("sqrt", "sqrt(float) -> float");
    expect("min", "min(float, float) -> float");
    expect("max", "max(float, float) -> float");
    expect("clamp", "clamp(float, float, float) -> float");
    expect("saturate", "saturate(float) -> float");
    expect("lerp", "lerp(float, float, float) -> float");
    expect("vec2", "vec2(float, float) -> vec2");
    expect("vec3", "vec3(float, float, float) -> vec3");
    expect("vec4", "vec4(float, float, float, float) -> vec4");
    expect("quat", "quat(float, float, float, float) -> quat");
    expect("dot", "dot(vec3, vec3) -> float");
    expect("cross", "cross(vec3, vec3) -> vec3");
    expect("length", "length(vec3) -> float");
    expect("length_squared", "length_squared(vec3) -> float");
    expect("normalize", "normalize(vec3) -> vec3");
    expect("distance", "distance(vec3, vec3) -> float");
    expect("distance_squared", "distance_squared(vec3, vec3) -> float");
    expect("rotate", "rotate(quat, vec3) -> vec3");
}

TEST_CASE("expr.fn: no overloads — one name, one signature, one registry entry") {
    std::set<std::string_view> names;
    for (const expr::Builtin& builtin : expr::builtins())
        CHECK(names.insert(builtin.name).second);
    // LIBM-BOUND transcendentals are excluded by policy (D-BUILD-019): they
    // land as det_* controlled implementations in core/math first, or not
    // at all. Pin the exclusion so an accidental libm entry fails loudly.
    for (const std::string_view banned :
         {"sin", "cos", "tan", "asin", "acos", "atan", "atan2", "exp", "log", "pow"})
        CHECK(expr::find_builtin(banned) == -1);
}

TEST_CASE("expr.fn: registry description carries the inventory into the API dump") {
    reflect::Registry registry;
    expr::register_expr_functions(registry);
    const auto* entry = registry.find_function(base::Name("clamp"));
    REQUIRE(entry != nullptr);
    const std::string json = reflect::describe(*entry).dump();
    CHECK(json.find("\"name\":\"clamp\"") != std::string::npos);
    CHECK(json.find("\"returns\":\"float\"") != std::string::npos);
    CHECK(json.find("\"compat_hash\":\"" + base::hex64(entry->desc.compat_hash) + "\"") !=
          std::string::npos);
}
