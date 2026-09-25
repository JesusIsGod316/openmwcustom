#include <components/render/native/staticworldservice.hpp>
#include <components/rendercore/renderworld.hpp>
#include <components/rendercore/updatebatch.hpp>

#include <cstdlib>
#include <memory>

namespace
{
    [[noreturn]] void fail() { std::abort(); }

    RenderCore::ModelHandle publishModel(RenderCore::RenderWorld& world, RenderCore::RenderWorldPublisher& publisher)
    {
        const auto handle = world.reserveModel();
        if (!handle)
            fail();

        auto payload = std::make_shared<RenderCore::ModelPayload>();
        RenderCore::ModelNodeRecord root;
        root.name = "root";
        payload->nodes.push_back(root);
        payload->roots.emplace_back(0u);

        RenderCore::ModelRecord record;
        record.sourceIdentity = "meshes/test.nif";
        record.contentIdentity = "test-content";
        record.bounds.minimum = {-1.f, -1.f, -1.f};
        record.bounds.maximum = {1.f, 1.f, 1.f};
        record.payload = std::move(payload);

        RenderCore::RenderWorldUpdateBatch batch(world.epoch(), publisher.nextSequence(), "phase2-smoke-model");
        if (!batch.add(RenderCore::CreateModel{*handle, std::move(record)}) || !batch.seal()
            || publisher.apply(batch) != RenderCore::PublishStatus::Applied)
            fail();
        return *handle;
    }
}

int main()
{
    RenderCore::RenderWorld world;
    RenderCore::RenderWorldPublisher publisher(world);
    RenderCore::ActiveCellProducer cells(world, publisher);
    RenderCore::StaticPopulationProducer populations(world, publisher);
    RenderNative::StaticWorldService staticWorld(cells, populations);

    const RenderCore::ModelHandle model = publishModel(world, publisher);

    RenderNative::StaticWorldCellSource interior;
    interior.cell.identity = "world:test/interior:one";
    interior.cell.worldspaceIdentity = "test";
    if (!staticWorld.activateCell(interior).accepted())
        fail();

    RenderCore::StaticInstanceSource source;
    source.identity = "ref:1";
    source.cellIdentity = interior.cell.identity;
    source.model = model;
    source.localBounds.minimum = {-1.f, -1.f, -1.f};
    source.localBounds.maximum = {1.f, 1.f, 1.f};
    if (!staticWorld.upsertStatic(source, false).accepted())
        fail();

    std::size_t instanceCount = 0;
    world.forEachInstance([&](auto, const auto&) { ++instanceCount; });
    if (instanceCount != 1)
        fail();

    RenderNative::StaticWorldCellSource exterior;
    exterior.cell.identity = "world:test/exterior:0,0";
    exterior.cell.worldspaceIdentity = "test";
    exterior.exterior = true;
    if (!staticWorld.activateCell(exterior).accepted())
        fail();

    source.cellIdentity = exterior.cell.identity;
    source.transform.translation = {8192.0, 8192.0, 0.0};
    if (!staticWorld.upsertStatic(source, true).accepted())
        fail();
    if (populations.flush() != RenderCore::StaticPopulationPublishStatus::Applied)
        fail();

    instanceCount = 0;
    std::size_t populationPlacements = 0;
    world.forEachInstance([&](auto, const auto&) { ++instanceCount; });
    world.forEachChunk([&](auto, const RenderCore::ChunkRecord& chunk) {
        if (!chunk.population)
            return;
        for (const auto& group : chunk.population->groups)
            populationPlacements += group.instances.size();
    });
    if (instanceCount != 0 || populationPlacements != 1)
        fail();

    if (!staticWorld.removeStatic(source.identity).accepted())
        fail();
    if (populations.flush() != RenderCore::StaticPopulationPublishStatus::Applied)
        fail();

    populationPlacements = 0;
    world.forEachChunk([&](auto, const RenderCore::ChunkRecord& chunk) {
        if (!chunk.population)
            return;
        for (const auto& group : chunk.population->groups)
            populationPlacements += group.instances.size();
    });
    if (populationPlacements != 0)
        fail();

    if (!staticWorld.deactivateCell(interior.cell.identity).accepted()
        || !staticWorld.deactivateCell(exterior.cell.identity).accepted())
        fail();

    return 0;
}
