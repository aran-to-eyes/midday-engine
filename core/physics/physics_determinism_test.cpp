// core/physics/physics_determinism_test.cpp — physics.min.determinism: the
// 64-box pile. Two INDEPENDENTLY constructed sims (fresh World, Hierarchy,
// Bus, Writer, TickLoop, PhysicsServer, fresh Jolt PhysicsSystem) run the
// identical operation script for 1000 ticks; a phase-6 hook journals every
// body's transform at a fixed stride. The two bundles' record streams must
// be BYTE-identical — a dual-run cmp, never a self-diff (AGENTS rule 5).

#include "core/base/file_io.h"
#include "core/physics/physics_server.h"
#include "core/physics/test_support.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace midday::physics {
namespace {

using test::PhysicsFixture;

constexpr std::uint64_t kPileTicks = 1000;
constexpr std::uint64_t kSnapshotStride = 100;

// Journals {bodies: [[id, tx,ty,tz, qx,qy,qz,qw], ...]} for every bound
// body, ascending body id, every kSnapshotStride ticks — cause: the phase-6
// marker. Registered AFTER the server on the physics phase, so it snapshots
// post-step, post-sync state (registration order is execution order).
struct SnapshotHook : tick::PhaseHook {
    PhysicsServer* server = nullptr;
    journal::Writer* writer = nullptr;
    std::vector<PhysicsBodyId>* bodies = nullptr;

    void on_phase(tick::TickLoop& /*loop*/, const tick::PhaseContext& context) override {
        if (context.tick % kSnapshotStride != 0)
            return;
        base::Json rows = base::Json::array();
        for (const PhysicsBodyId id : *bodies) {
            const auto found = server->body_transform(id);
            const math::Transform& transform = testkit::unwrap(found);
            base::Json row = base::Json::array();
            row.push(base::Json(id.value));
            row.push(base::Json(static_cast<double>(transform.translation.x)));
            row.push(base::Json(static_cast<double>(transform.translation.y)));
            row.push(base::Json(static_cast<double>(transform.translation.z)));
            row.push(base::Json(static_cast<double>(transform.rotation.x)));
            row.push(base::Json(static_cast<double>(transform.rotation.y)));
            row.push(base::Json(static_cast<double>(transform.rotation.z)));
            row.push(base::Json(static_cast<double>(transform.rotation.w)));
            rows.push(std::move(row));
        }
        base::Json payload = base::Json::object();
        payload.set("bodies", std::move(rows));
        const std::uint64_t id = writer->record(context.tick,
                                                journal::Tier::Flight,
                                                "physics.transforms",
                                                context.phase_record_id,
                                                std::move(payload));
        REQUIRE(id != 0);
    }
};

// One full pile run under its own TempDir; returns the bundle's compressed
// record-stream bytes (header.json is identical by pinned config).
std::string run_pile() {
    PhysicsFixture fixture;
    PhysicsServer& server = *fixture.server;

    const ecs::EntityRef ground = fixture.adopted_entity();
    REQUIRE_FALSE(server.create_ground_plane(ground, 0.0F).is_null());

    // 64 boxes: a 4x4x4 grid, slightly separated so the pile collapses and
    // grinds through real contact resolution for the whole run.
    std::vector<PhysicsBodyId> bodies;
    bodies.push_back(server.body_of(ground));
    for (int layer = 0; layer < 4; ++layer)
        for (int row = 0; row < 4; ++row)
            for (int col = 0; col < 4; ++col) {
                const math::Vec3 position{-1.65F + 1.1F * static_cast<float>(col),
                                          0.55F + 1.1F * static_cast<float>(layer),
                                          -1.65F + 1.1F * static_cast<float>(row)};
                const ecs::EntityRef box = fixture.drop_box(position);
                bodies.push_back(server.body_of(box));
            }
    REQUIRE(server.body_count() == 65);

    SnapshotHook snapshot;
    snapshot.server = &server;
    snapshot.writer = &fixture.sim.writer();
    snapshot.bodies = &bodies;
    REQUIRE_FALSE(fixture.sim.loop().add_hook(tick::Phase::kPhysics, snapshot).has_value());

    REQUIRE_FALSE(fixture.sim.loop().tick(kPileTicks).has_value());
    CHECK(fixture.server->stats().steps == kPileTicks);
    CHECK(fixture.server->stats().sync_writes == kPileTicks * 64);

    REQUIRE_FALSE(fixture.sim.writer().close().has_value());
    auto read = base::read_file(fixture.sim.bundle_path() + "/journal.jsonl.zst", "physics.io");
    REQUIRE_FALSE(read.error.has_value());
    return std::move(read.bytes);
}

TEST_CASE("physics.min.determinism: 64-box pile, 1000 ticks, dual-run byte-identical") {
    const std::string first = run_pile();
    const std::string second = run_pile();
    REQUIRE_FALSE(first.empty());
    CHECK(first.size() == second.size());
    CHECK(first == second);
}

} // namespace
} // namespace midday::physics
