// ecs.pool — typed pools (core/ecs/pool.h): value/dense mirroring under
// swap-and-pop, and THE zero-move guarantee: toggling active bits changes
// no addresses and no order (measured on the actual pointers).

#include "core/base/name.h"
#include "core/ecs/pool.h"
#include "testkit/doctest.h"

#include <cstdint>
#include <vector>

using midday::base::Name;
using midday::ecs::Pool;

namespace {

struct Payload {
    std::uint32_t value = 0;
};

} // namespace

TEST_CASE("ecs.pool: values mirror the dense order through erase") {
    Pool<Payload> pool(Name("Payload"));
    for (std::uint32_t e : {5u, 6u, 7u, 8u})
        pool.emplace(e, Payload{e * 100}, true);

    REQUIRE(pool.try_get(6) != nullptr);
    CHECK(pool.try_get(6)->value == 600);

    // Erase a middle row: the last value must follow its entity.
    CHECK(pool.erase(6));
    CHECK(pool.set().dense() == std::vector<std::uint32_t>{5, 8, 7});
    CHECK(pool.try_get(8)->value == 800);
    CHECK(pool.try_get(6) == nullptr);
    CHECK(pool.data().size() == 3);

    CHECK_FALSE(pool.erase(6)); // absent: reports false, despawn path tolerates
}

TEST_CASE("ecs.pool: toggling active bits moves ZERO memory") {
    Pool<Payload> pool(Name("Payload"));
    for (std::uint32_t e = 0; e < 200; ++e)
        pool.emplace(e, Payload{e}, true);

    // Address + order anchors before any toggling.
    const Payload* data_before = pool.data().data();
    const std::uint32_t* dense_before = pool.set().dense().data();
    const std::vector<std::uint32_t> order_before = pool.set().dense();
    const Payload* row_17 = pool.try_get(17);
    const Payload* row_150 = pool.try_get(150);

    // Toggle hard: every row off, half back on, then churn one row 1000x.
    for (std::uint32_t e = 0; e < 200; ++e)
        pool.set_row_active(pool.set().find(e), false);
    for (std::uint32_t e = 0; e < 200; e += 2)
        pool.set_row_active(pool.set().find(e), true);
    for (int i = 0; i < 1000; ++i)
        pool.set_row_active(pool.set().find(17), (i & 1) == 0);

    // The measurement: same buffers, same rows, same order — bits only.
    CHECK(pool.data().data() == data_before);
    CHECK(pool.set().dense().data() == dense_before);
    CHECK(pool.set().dense() == order_before);
    CHECK(pool.try_get(17) == row_17);
    CHECK(pool.try_get(150) == row_150);
    CHECK(pool.try_get(17)->value == 17);

    // And the bits themselves landed where directed.
    CHECK_FALSE(pool.set().is_active(pool.set().find(17)));
    CHECK(pool.set().is_active(pool.set().find(150)));
    CHECK_FALSE(pool.set().is_active(pool.set().find(151)));
}
