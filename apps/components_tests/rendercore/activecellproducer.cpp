#include <components/rendercore/activecellproducer.hpp>

#include <gtest/gtest.h>

#include <memory>

namespace
{
    RenderCore::ModelHandle addModel(
        RenderCore::RenderWorld& world, RenderCore::RenderWorldPublisher& publisher)
    {
        const auto handle = world.reserveModel();
        EXPECT_TRUE(handle);
        if (!handle)
            return {};
        RenderCore::ModelRecord record;
        record.sourceIdentity = "meshes/mod-winner.nif";
        record.contentIdentity = "sha256:test";
        record.payload = std::make_shared<RenderCore::ModelPayload>();
        RenderCore::RenderWorldUpdateBatch batch(
            world.epoch(), publisher.nextSequence(), record.sourceIdentity);
        EXPECT_TRUE(batch.add(RenderCore::CreateModel{ *handle, std::move(record) }));
        EXPECT_TRUE(batch.seal());
        EXPECT_EQ(publisher.apply(batch), RenderCore::PublishStatus::Applied);
        return *handle;
    }

    TEST(ActiveCellProducer, MaintainsStableIdentityAndAtomicCellOwnership)
    {
        RenderCore::RenderWorld world;
        RenderCore::RenderWorldPublisher publisher(world);
        RenderCore::ActiveCellProducer producer(world, publisher);
        const RenderCore::ModelHandle model = addModel(world, publisher);
        ASSERT_TRUE(model.valid());

        const RenderCore::ActiveCellSource first{ .identity = "cell:first", .worldspaceIdentity = "world" };
        const RenderCore::ActiveCellSource second{ .identity = "cell:second", .worldspaceIdentity = "world" };
        ASSERT_TRUE(producer.addCell(first).applied());
        ASSERT_TRUE(producer.addCell(second).applied());

        RenderCore::StaticInstanceSource source;
        source.identity = "content:0/ref:42";
        source.cellIdentity = first.identity;
        source.model = model;
        source.transform.translation = { 1.0, 2.0, 3.0 };
        const auto created = producer.upsertStaticInstance(source);
        ASSERT_TRUE(created.applied());

        source.transform.translation.x = 4.0;
        const auto updated = producer.upsertStaticInstance(source);
        ASSERT_TRUE(updated.applied());
        EXPECT_EQ(updated.instance, created.instance);
        EXPECT_EQ(world.get(created.instance)->revision, RenderCore::ResourceRevision{ 2 });

        source.cellIdentity = second.identity;
        const auto moved = producer.upsertStaticInstance(source);
        ASSERT_TRUE(moved.applied());
        const auto firstCell = producer.findCell(first.identity);
        const auto secondCell = producer.findCell(second.identity);
        ASSERT_TRUE(firstCell);
        ASSERT_TRUE(secondCell);
        EXPECT_EQ(moved.instance, created.instance);
        EXPECT_EQ(world.get(created.instance)->revision, RenderCore::ResourceRevision{ 4 });
        EXPECT_EQ(world.get(created.instance)->chunk, secondCell);
        EXPECT_TRUE(world.get(*firstCell)->members.empty());

        EXPECT_TRUE(producer.removeCell(second.identity).applied());
        EXPECT_EQ(world.get(created.instance), nullptr);
        EXPECT_EQ(producer.instanceCount(), 0u);
        EXPECT_TRUE(world.valid());
    }

    TEST(ActiveCellProducer, FailsClosedOnMissingDependenciesAndResetsAtWorldEpoch)
    {
        RenderCore::RenderWorld world;
        RenderCore::RenderWorldPublisher publisher(world);
        RenderCore::ActiveCellProducer producer(world, publisher);
        const RenderCore::ModelHandle model = addModel(world, publisher);
        ASSERT_TRUE(model.valid());

        RenderCore::StaticInstanceSource source;
        source.identity = "content:0/ref:7";
        source.cellIdentity = "cell:missing";
        source.model = model;
        EXPECT_EQ(producer.upsertStaticInstance(source).status, RenderCore::ActiveCellPublishStatus::MissingCell);
        EXPECT_EQ(world.instanceCount(), 0u);

        ASSERT_TRUE(producer.addCell({ .identity = source.cellIdentity }).applied());
        ASSERT_TRUE(world.reset());
        ASSERT_TRUE(producer.addCell({ .identity = source.cellIdentity }).applied());
        EXPECT_EQ(producer.upsertStaticInstance(source).status, RenderCore::ActiveCellPublishStatus::MissingModel);
        EXPECT_EQ(producer.instanceCount(), 0u);
        EXPECT_TRUE(world.valid());
    }
}
