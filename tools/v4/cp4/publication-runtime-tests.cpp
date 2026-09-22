#include <components/rendercore/updatebatch.hpp>
#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace RenderCore;
static void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
int main()
{
    try
    {
        RenderWorld world;
        RenderWorldPublisher publisher(world);
        auto publish = [&](RenderWorldUpdateOperation operation) {
            RenderWorldUpdateBatch batch(world.epoch(), publisher.nextSequence());
            require(batch.add(std::move(operation)) && batch.seal(), "cannot seal batch");
            return publisher.apply(batch);
        };
        // A large, immutable mesh makes accidental world-wide rescans visible.
        auto meshPayload = std::make_shared<MeshPayload>();
        meshPayload->positions.assign(300000, glm::vec3(1, 2, 3));
        MeshRecord mesh; mesh.payload = meshPayload;
        const auto mh = world.reserveMesh();
        require(mh && publish(CreateMesh{*mh, mesh}) == PublishStatus::Applied, "mesh create failed");
        meshPayload.reset(); // published payloads are immutable from here
        const auto lh = world.reserveLight();
        require(lh && publish(CreateLight{*lh, {}}) == PublishStatus::Applied, "light create failed");

        auto beforeRevision = world.revision();
        auto beforeSequence = publisher.lastSequence();
        LightRecord invalid = *world.get(*lh);
        invalid.revision = *advanceMonotonic(invalid.revision);
        invalid.effectiveRadius = -1;
        require(publish(UpdateLight{*lh, invalid}) == PublishStatus::OperationRejected, "invalid light accepted");
        require(world.revision() == beforeRevision && publisher.lastSequence() == beforeSequence
            && world.get(*lh)->effectiveRadius == 0, "failed light update mutated world");

        // A later failed operation must roll back an earlier successful one.
        RenderWorldUpdateBatch mixed(world.epoch(), publisher.nextSequence());
        LightRecord changed = *world.get(*lh);
        changed.revision = *advanceMonotonic(changed.revision); changed.effectiveRadius = 42;
        MeshRecord badMesh = mesh;
        auto badPayload = std::make_shared<MeshPayload>();
        badPayload->positions.push_back({std::numeric_limits<float>::quiet_NaN(), 0, 0});
        badMesh.payload = badPayload; badMesh.revision = *advanceMonotonic(mesh.revision);
        require(mixed.add(UpdateLight{*lh, changed}) && mixed.add(UpdateMesh{*mh, badMesh}) && mixed.seal(), "mixed batch build");
        require(publisher.apply(mixed) == PublishStatus::OperationRejected && world.revision() == beforeRevision
            && world.get(*lh)->effectiveRadius == 0, "mixed batch lost rollback");

        const auto start = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < 100; ++i)
        {
            LightRecord record = *world.get(*lh);
            record.revision = *advanceMonotonic(record.revision); record.position.x = i;
            require(publish(UpdateLight{*lh, record}) == PublishStatus::Applied, "moving light update failed");
        }
        const auto duration = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        require(world.valid() && world.get(*lh)->position.x == 99, "final independent audit failed");
        std::cout << "100 light publications with 300000 immutable vertices: " << duration << " ms; mode="
            << (std::getenv("OPENMW_V4_FULL_WORLD_PUBLICATION") ? "full-control" : "runtime") << '\n';
        const auto loadingStart = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < 24; ++i)
        {
            const auto handle = world.reserveMesh();
            require(handle && publish(CreateMesh{*handle, mesh}) == PublishStatus::Applied, "incremental mesh publication failed");
        }
        std::cout << "24 mesh publications with 300000 vertices each: "
            << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - loadingStart).count() << " ms\n";
        require(world.valid(), "incremental loading failed full audit");
        require(publish(RetireLight{*lh}) == PublishStatus::Applied && !world.get(*lh), "light retirement failed");
        beforeRevision = world.revision(); beforeSequence = publisher.lastSequence();
        require(publish(UpdateLight{*lh, changed}) == PublishStatus::OperationRejected
            && world.revision() == beforeRevision && publisher.lastSequence() == beforeSequence,
            "stale light handle mutated state");
        // A reset must restart the same publisher's epoch/sequence contract.
        require(world.reset(), "world reset failed");
        const auto nextLight = world.reserveLight();
        require(nextLight && publish(CreateLight{*nextLight, {}}) == PublishStatus::Applied && world.valid(),
            "publication after reset failed");
        std::cout << "Publication rejection, rollback, retirement and reset: PASS\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
