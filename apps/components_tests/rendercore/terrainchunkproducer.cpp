#include <components/rendercore/terrainchunkproducer.hpp>

#include <gtest/gtest.h>

#include <memory>

namespace
{
    RenderCore::TerrainChunkSource makeSource(std::string identity, std::int32_t x, std::int32_t y)
    {
        auto mesh = std::make_shared<RenderCore::MeshPayload>();
        mesh->positions = { { -1.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f }, { 1.0f, 1.0f, 0.0f } };
        mesh->normals.assign(3, glm::vec3(0.0f, 0.0f, 1.0f));
        mesh->colors.assign(3, glm::vec4(1.0f));
        mesh->indices = { 0, 1, 2 };
        mesh->surfaces.push_back(RenderCore::MeshSurface{ .indexCount = 3 });

        RenderCore::TerrainChunkSource source;
        source.identity = std::move(identity);
        source.worldspaceIdentity = "world";
        source.gridX = x;
        source.gridY = y;
        source.transform.translation = { static_cast<double>(x * 2), static_cast<double>(y * 2), 0.0 };
        source.localBounds.minimum = { -1.0f, -1.0f, 0.0f };
        source.localBounds.maximum = { 1.0f, 1.0f, 0.0f };
        source.mesh = std::move(mesh);
        source.material.vertexColorMode = RenderCore::VertexColorMode::AmbientDiffuse;
        return source;
    }

    TEST(TerrainChunkProducer, PublishesReplacesAndRetiresOneNeutralSurface)
    {
        RenderCore::RenderWorld world;
        RenderCore::RenderWorldPublisher publisher(world);
        RenderCore::TerrainChunkProducer producer(world, publisher);

        EXPECT_EQ(
            producer.synchronize(makeSource("terrain:0,0", 0, 0)), RenderCore::TerrainChunkPublishStatus::Applied);
        EXPECT_EQ(world.chunkCount(), 1u);
        EXPECT_EQ(world.instanceCount(), 1u);
        EXPECT_TRUE(world.valid());

        world.forEachChunk([&](RenderCore::ChunkHandle, const RenderCore::ChunkRecord& chunk) {
            EXPECT_EQ(chunk.kind, RenderCore::ChunkRecord::Kind::Terrain);
            EXPECT_EQ(chunk.gridX, 0);
            EXPECT_EQ(chunk.gridY, 0);
            ASSERT_EQ(chunk.members.size(), 1u);
            const RenderCore::InstanceRecord* instance = world.get(chunk.members.front());
            ASSERT_NE(instance, nullptr);
            EXPECT_NE(
                instance->semanticFlags & RenderCore::semanticFlag(RenderCore::InstanceSemanticFlag::Terrain), 0u);
        });
        EXPECT_EQ(producer.synchronize(makeSource("terrain:0,0", 0, 0)),
            RenderCore::TerrainChunkPublishStatus::AlreadyPresent);
        EXPECT_TRUE(producer.contains("terrain:0,0"));
        EXPECT_FALSE(producer.contains("terrain:1,0"));

        EXPECT_EQ(
            producer.synchronize(makeSource("terrain:1,0", 1, 0)), RenderCore::TerrainChunkPublishStatus::Applied);
        EXPECT_EQ(producer.activeIdentity(), "terrain:1,0");
        EXPECT_EQ(world.chunkCount(), 1u);
        EXPECT_EQ(world.instanceCount(), 1u);
        EXPECT_TRUE(world.valid());

        EXPECT_EQ(producer.synchronize(std::nullopt), RenderCore::TerrainChunkPublishStatus::Applied);
        EXPECT_TRUE(producer.activeIdentity().empty());
        EXPECT_EQ(world.chunkCount(), 0u);
        EXPECT_EQ(world.instanceCount(), 0u);
        EXPECT_TRUE(world.valid());
    }

    TEST(TerrainChunkProducer, RejectsMalformedPayloadWithoutPublishing)
    {
        RenderCore::RenderWorld world;
        RenderCore::RenderWorldPublisher publisher(world);
        RenderCore::TerrainChunkProducer producer(world, publisher);
        RenderCore::TerrainChunkSource source = makeSource("terrain:bad", 0, 0);
        auto malformed = std::make_shared<RenderCore::MeshPayload>(*source.mesh);
        malformed->indices.push_back(99);
        source.mesh = std::move(malformed);

        EXPECT_EQ(producer.synchronize(source), RenderCore::TerrainChunkPublishStatus::InvalidSource);
        EXPECT_EQ(world.chunkCount(), 0u);
        EXPECT_EQ(world.instanceCount(), 0u);
        EXPECT_TRUE(world.valid());
    }
}
