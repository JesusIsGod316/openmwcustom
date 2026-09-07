#include <components/rendercore/activecellproducer.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>

namespace
{
    bool require(bool condition, std::string_view message)
    {
        if (!condition)
            std::cerr << "CP3C active-cell producer failure: " << message << '\n';
        return condition;
    }

    RenderCore::ModelHandle addEmptyModel(
        RenderCore::RenderWorld& world, RenderCore::RenderWorldPublisher& publisher)
    {
        const auto handle = world.reserveModel();
        if (!handle)
            return {};
        auto payload = std::make_shared<RenderCore::ModelPayload>();
        RenderCore::ModelRecord record;
        record.sourceIdentity = "meshes/mod-winner.nif";
        record.contentIdentity = "sha256:test";
        record.payload = std::move(payload);
        RenderCore::RenderWorldUpdateBatch batch(
            world.epoch(), RenderCore::InitialUpdateSequence, record.sourceIdentity);
        if (!batch.add(RenderCore::CreateModel{ *handle, std::move(record) }) || !batch.seal()
            || publisher.apply(batch) != RenderCore::PublishStatus::Applied)
        {
            world.cancel(*handle);
            return {};
        }
        return *handle;
    }
}

int main()
{
    RenderCore::RenderWorld world;
    RenderCore::RenderWorldPublisher publisher(world);
    RenderCore::ActiveCellProducer producer(world, publisher);
    const RenderCore::ModelHandle model = addEmptyModel(world, publisher);
    if (!require(model.valid(), "model fixture"))
        return EXIT_FAILURE;

    RenderCore::ActiveCellSource balmora;
    balmora.identity = "world:morrowind/cell:0,-2";
    balmora.worldspaceIdentity = "morrowind";
    RenderCore::ActiveCellSource pelagiad;
    pelagiad.identity = "world:morrowind/cell:0,-3";
    pelagiad.worldspaceIdentity = "morrowind";
    if (!require(producer.addCell(balmora).applied(), "add first active cell")
        || !require(producer.addCell(pelagiad).applied(), "add second active cell")
        || !require(producer.addCell(balmora).status == RenderCore::ActiveCellPublishStatus::AlreadyPresent,
            "duplicate cell is idempotent"))
        return EXIT_FAILURE;

    RenderCore::StaticInstanceSource source;
    source.identity = "content:0/ref:42";
    source.cellIdentity = balmora.identity;
    source.model = model;
    source.transform.translation = { 10.0, 20.0, 30.0 };
    const auto created = producer.upsertStaticInstance(source);
    if (!require(created.applied() && created.instance.valid(), "create static reference")
        || !require(world.instanceCount() == 1 && world.chunkCount() == 2, "published population"))
        return EXIT_FAILURE;

    source.transform.translation.x = 40.0;
    const auto updated = producer.upsertStaticInstance(source);
    const RenderCore::InstanceRecord* updatedRecord = world.get(created.instance);
    if (!require(updated.applied() && updated.instance == created.instance, "stable handle on transform update")
        || !require(updatedRecord && updatedRecord->revision == RenderCore::ResourceRevision{ 2 }
                && updatedRecord->transform.translation.x == 40.0,
            "revisioned transform update"))
        return EXIT_FAILURE;

    source.cellIdentity = pelagiad.identity;
    source.transform.translation.y = 50.0;
    const auto moved = producer.upsertStaticInstance(source);
    const auto balmoraHandle = producer.findCell(balmora.identity);
    const auto pelagiadHandle = producer.findCell(pelagiad.identity);
    const RenderCore::InstanceRecord* movedRecord = world.get(created.instance);
    if (!require(moved.applied() && moved.instance == created.instance, "stable handle on cell move")
        || !require(movedRecord && movedRecord->revision == RenderCore::ResourceRevision{ 4 }
                && movedRecord->chunk == pelagiadHandle && movedRecord->transform.translation.y == 50.0,
            "atomic reparent and transform update")
        || !require(balmoraHandle && world.get(*balmoraHandle)->members.empty(), "old cell reverse index cleared")
        || !require(pelagiadHandle && world.get(*pelagiadHandle)->members.size() == 1, "new cell reverse index set"))
        return EXIT_FAILURE;

    if (!require(producer.removeCell(balmora.identity).applied(), "remove empty active cell")
        || !require(producer.removeCell(pelagiad.identity).applied(), "cell unload retires owned references")
        || !require(world.instanceCount() == 0 && world.chunkCount() == 0 && producer.instanceCount() == 0,
            "unload population cleanup")
        || !require(!world.get(created.instance), "removed generation is stale"))
        return EXIT_FAILURE;

    if (!require(world.reset(), "world epoch reset")
        || !require(producer.addCell(balmora).applied(), "producer resynchronizes after epoch reset")
        || !require(producer.cellCount() == 1 && producer.instanceCount() == 0, "epoch clears source bindings"))
        return EXIT_FAILURE;

    source.cellIdentity = balmora.identity;
    if (!require(producer.upsertStaticInstance(source).status == RenderCore::ActiveCellPublishStatus::MissingModel,
            "stale pre-reset model rejected"))
        return EXIT_FAILURE;

    std::cout << "V4 CP3C active-cell semantic producer: PASS\n";
    return EXIT_SUCCESS;
}
