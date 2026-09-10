#include <components/rendercore/staticpopulationproducer.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string_view>

namespace
{
    RenderCore::ModelHandle addModel(RenderCore::RenderWorld& world, RenderCore::RenderWorldPublisher& publisher,
        std::string_view identity)
    {
        const auto handle = world.reserveModel();
        EXPECT_TRUE(handle);
        if (!handle)
            return {};
        RenderCore::ModelRecord record;
        record.sourceIdentity = identity;
        record.contentIdentity = std::string(identity) + ":content";
        record.payload = std::make_shared<RenderCore::ModelPayload>();
        RenderCore::RenderWorldUpdateBatch batch(world.epoch(), publisher.nextSequence(), record.sourceIdentity);
        EXPECT_TRUE(batch.add(RenderCore::CreateModel{ *handle, std::move(record) }));
        EXPECT_TRUE(batch.seal());
        EXPECT_EQ(publisher.apply(batch), RenderCore::PublishStatus::Applied);
        return *handle;
    }

    const RenderCore::ChunkRecord* onlyChunk(const RenderCore::RenderWorld& world)
    {
        const RenderCore::ChunkRecord* result = nullptr;
        world.forEachChunk([&](RenderCore::ChunkHandle, const RenderCore::ChunkRecord& record) { result = &record; });
        return result;
    }

    RenderCore::StaticPopulationInstanceSource instance(
        std::string identity, RenderCore::ModelHandle model, double x = 0.0)
    {
        RenderCore::StaticPopulationInstanceSource result;
        result.identity = std::move(identity);
        result.cellIdentity = "cell:0,0";
        result.model = model;
        result.transform.translation.x = x;
        return result;
    }

    TEST(StaticPopulationProducer, PublishesDeterministicModelGroupedCellPayload)
    {
        RenderCore::RenderWorld world;
        RenderCore::RenderWorldPublisher publisher(world);
        RenderCore::StaticPopulationProducer producer(world, publisher);
        const RenderCore::ModelHandle tree = addModel(world, publisher, "meshes/tree.nif");
        const RenderCore::ModelHandle rock = addModel(world, publisher, "meshes/rock.nif");
        ASSERT_TRUE(tree.valid() && rock.valid());

        ASSERT_EQ(producer.addCell({ .identity = "cell:0,0", .worldspaceIdentity = "world" }),
            RenderCore::StaticPopulationPublishStatus::Applied);
        ASSERT_EQ(producer.upsert(instance("ref:z", tree, 3.0)), RenderCore::StaticPopulationPublishStatus::Applied);
        ASSERT_EQ(producer.upsert(instance("ref:a", rock, 1.0)), RenderCore::StaticPopulationPublishStatus::Applied);
        ASSERT_EQ(producer.upsert(instance("ref:b", tree, 2.0)), RenderCore::StaticPopulationPublishStatus::Applied);
        ASSERT_EQ(producer.flush(), RenderCore::StaticPopulationPublishStatus::Applied);

        ASSERT_EQ(world.chunkCount(), 1u);
        EXPECT_EQ(world.instanceCount(), 0u);
        const RenderCore::ChunkRecord* chunk = onlyChunk(world);
        ASSERT_NE(chunk, nullptr);
        ASSERT_NE(chunk->population, nullptr);
        ASSERT_EQ(chunk->population->groups.size(), 2u);
        EXPECT_EQ(chunk->population->groups[0].model, tree);
        ASSERT_EQ(chunk->population->groups[0].instances.size(), 2u);
        EXPECT_EQ(chunk->population->groups[0].instances[0].sourceIdentity, "ref:b");
        EXPECT_EQ(chunk->population->groups[0].instances[1].sourceIdentity, "ref:z");
        EXPECT_EQ(chunk->population->groups[1].model, rock);
        ASSERT_EQ(chunk->population->groups[1].instances.size(), 1u);
        EXPECT_EQ(chunk->population->groups[1].instances[0].sourceIdentity, "ref:a");
        EXPECT_TRUE(world.valid());
    }

    TEST(StaticPopulationProducer, ReplacesPayloadAndRetiresEmptyCellAtomically)
    {
        RenderCore::RenderWorld world;
        RenderCore::RenderWorldPublisher publisher(world);
        RenderCore::StaticPopulationProducer producer(world, publisher);
        const RenderCore::ModelHandle model = addModel(world, publisher, "meshes/tree.nif");
        ASSERT_TRUE(model.valid());
        ASSERT_EQ(producer.addCell({ .identity = "cell:0,0", .worldspaceIdentity = "world" }),
            RenderCore::StaticPopulationPublishStatus::Applied);

        auto source = instance("ref:1", model, 1.0);
        ASSERT_EQ(producer.upsert(source), RenderCore::StaticPopulationPublishStatus::Applied);
        ASSERT_EQ(producer.flush(), RenderCore::StaticPopulationPublishStatus::Applied);
        ASSERT_EQ(onlyChunk(world)->revision, RenderCore::ResourceRevision{ 1 });

        source.transform.translation.x = 7.0;
        ASSERT_EQ(producer.upsert(source), RenderCore::StaticPopulationPublishStatus::Applied);
        ASSERT_EQ(producer.flush(), RenderCore::StaticPopulationPublishStatus::Applied);
        ASSERT_NE(onlyChunk(world), nullptr);
        EXPECT_EQ(onlyChunk(world)->revision, RenderCore::ResourceRevision{ 2 });
        EXPECT_DOUBLE_EQ(onlyChunk(world)->population->groups[0].instances[0].transform.translation.x, 7.0);

        EXPECT_FALSE(world.retire(model));
        ASSERT_EQ(producer.remove("ref:1"), RenderCore::StaticPopulationPublishStatus::Applied);
        ASSERT_EQ(producer.flush(), RenderCore::StaticPopulationPublishStatus::Applied);
        EXPECT_EQ(world.chunkCount(), 0u);
        EXPECT_TRUE(world.retire(model));
        EXPECT_TRUE(world.valid());
    }

    TEST(StaticPopulationProducer, MarksEveryGroundcoverPlacementAndResetsWithWorldEpoch)
    {
        RenderCore::RenderWorld world;
        RenderCore::RenderWorldPublisher publisher(world);
        RenderCore::StaticPopulationProducer producer(world, publisher);
        const RenderCore::ModelHandle model = addModel(world, publisher, "meshes/grass.nif");
        ASSERT_TRUE(model.valid());
        ASSERT_EQ(producer.addCell(
                      { .identity = "cell:0,0", .worldspaceIdentity = "world", .groundcover = true }),
            RenderCore::StaticPopulationPublishStatus::Applied);
        ASSERT_EQ(producer.upsert(instance("grass:1", model)), RenderCore::StaticPopulationPublishStatus::Applied);
        ASSERT_EQ(producer.flush(), RenderCore::StaticPopulationPublishStatus::Applied);

        const RenderCore::ChunkRecord* chunk = onlyChunk(world);
        ASSERT_NE(chunk, nullptr);
        ASSERT_EQ(chunk->kind, RenderCore::ChunkRecord::Kind::Groundcover);
        ASSERT_EQ(chunk->population->groups.size(), 1u);
        EXPECT_NE(chunk->population->groups[0].instances[0].semanticFlags
                & RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::Groundcover),
            0u);

        ASSERT_TRUE(world.reset());
        EXPECT_EQ(producer.stagedInstanceCount(), 0u);
        EXPECT_EQ(producer.remove("grass:1"), RenderCore::StaticPopulationPublishStatus::AlreadyPresent);
    }
}
